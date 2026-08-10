/*
 * Wii BNS (banner sound) demuxer
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * The jingle a Wii channel plays on its banner. Structurally it is a small
 * chunked container -- header, INFO, DATA -- carrying DSP-ADPCM, with each
 * channel stored as one contiguous run rather than interleaved in blocks the
 * way BRSTM does it.
 *
 * In the wild these rarely turn up bare. They sit inside a banner archive, so
 * a file usually begins with an IMD5 header, is LZ10-compressed, or both, in
 * either order. Rather than make that the user's problem, the demuxer peels
 * the wrappers off in a loop and reads what is underneath.
 */

#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/nintendo_lz.h"

#include "avformat.h"
#include "avio_internal.h"
#include "bns.h"
#include "demux.h"
#include "dsp_adpcm.h"
#include "internal.h"

/* Bare .bnr banner sounds run to a couple of MB; this only bounds what a
 * corrupt LZ10 size field can make us allocate. */
#define BNS_MAX_SIZE (64 << 20)

typedef struct BNSDemuxContext {
    uint8_t *buf;           /* whole stream, unwrapped */
    int      buf_size;

    int64_t  ch_offset[FF_DSP_ADPCM_MAX_CHANNELS];
    int      channels;
    int64_t  samples_left;
    int64_t  pos;           /* frames consumed, per channel */
} BNSDemuxContext;

static int bns_probe(const AVProbeData *p)
{
    if (p->buf_size >= 8 && AV_RL32(p->buf) == MKTAG('B','N','S',' ') &&
        AV_RB32(p->buf + 4) == BNS_VERSION)
        return AVPROBE_SCORE_MAX;

    /* A compressed or IMD5-wrapped file says nothing about its contents until
     * it is unwrapped, and unwrapping is too expensive for a probe. Recognise
     * those by name only. */
    if (p->buf_size >= 4 &&
        (AV_RL32(p->buf) == MKTAG('I','M','D','5') ||
         p->buf[0] == AV_NINTENDO_LZ10_TAG) &&
        av_match_ext(p->filename, "bns"))
        return AVPROBE_SCORE_EXTENSION;

    return 0;
}

/* Strip IMD5 headers and LZ10 compression until a BNS header shows up. Both
 * wrappers can appear, in either order, so this loops rather than testing
 * once. */
static int bns_unwrap(AVFormatContext *s, uint8_t **bufp, int *sizep)
{
    uint8_t *buf = *bufp;
    int size = *sizep;

    for (int guard = 0; guard < 4; guard++) {
        if (size >= 8 && AV_RL32(buf) == MKTAG('B','N','S',' ')) {
            *bufp = buf;
            *sizep = size;
            return 0;
        }

        if (size > BNS_IMD5_SIZE && AV_RL32(buf) == MKTAG('I','M','D','5')) {
            memmove(buf, buf + BNS_IMD5_SIZE, size - BNS_IMD5_SIZE);
            size -= BNS_IMD5_SIZE;
            continue;
        }

        if (size > 4 && buf[0] == AV_NINTENDO_LZ10_TAG) {
            int out_size = avpriv_nintendo_lz10_size(buf, size);
            uint8_t *out;

            if (out_size < 8 || out_size > BNS_MAX_SIZE) {
                av_log(s, AV_LOG_ERROR, "implausible LZ10 size %d\n", out_size);
                return AVERROR_INVALIDDATA;
            }
            out = av_malloc(out_size);
            if (!out)
                return AVERROR(ENOMEM);
            out_size = avpriv_nintendo_lz10_decompress(buf, size, out, out_size);
            if (out_size < 0) {
                av_free(out);
                return out_size;
            }
            av_free(buf);
            buf  = out;
            size = out_size;
            continue;
        }

        break;
    }

    av_log(s, AV_LOG_ERROR, "no BNS header found\n");
    *bufp = buf;
    *sizep = size;
    return AVERROR_INVALIDDATA;
}

/* Read the whole stream. BNS files are small, and every offset in them is
 * relative to somewhere else in the file -- with compression in the mix there
 * is nothing to seek within anyway, so parsing from memory is simpler than
 * chasing offsets through an AVIOContext. */
static int bns_read_all(AVFormatContext *s, int64_t cap,
                        uint8_t **bufp, int *sizep)
{
    uint8_t *buf = NULL;
    int size = 0, alloc = 0;

    while (size < cap) {
        int want, got;

        if (size == alloc) {
            uint8_t *nb;
            alloc = alloc ? FFMIN(alloc * 2, (int)FFMIN(cap, BNS_MAX_SIZE))
                          : FFMIN(1 << 16, (int)cap);
            if (alloc <= size) {
                av_free(buf);
                return AVERROR_INVALIDDATA;
            }
            nb = av_realloc(buf, alloc);
            if (!nb) {
                av_free(buf);
                return AVERROR(ENOMEM);
            }
            buf = nb;
        }
        want = alloc - size;
        got  = avio_read(s->pb, buf + size, want);
        if (got <= 0)
            break;
        size += got;
    }

    if (size < 8) {
        av_free(buf);
        return AVERROR_INVALIDDATA;
    }
    *bufp  = buf;
    *sizep = size;
    return 0;
}

static int bns_read_header(AVFormatContext *s)
{
    BNSDemuxContext *c = s->priv_data;
    AVStream *st;
    int64_t file_len = avio_size(s->pb);
    int64_t info = 0, data = 0, list;
    uint32_t sample_count, loop_start;
    int chunk_count, header_size, ret;

    if (file_len <= 0 || file_len > BNS_MAX_SIZE) {
        /* Non-seekable or absurd: read what there is, bounded. */
        file_len = BNS_MAX_SIZE;
    }

    ret = bns_read_all(s, file_len, &c->buf, &c->buf_size);
    if (ret < 0)
        return ret;

    ret = bns_unwrap(s, &c->buf, &c->buf_size);
    if (ret < 0)
        return ret;

    if (c->buf_size < 0x10 || AV_RB32(c->buf + 4) != BNS_VERSION)
        return AVERROR_INVALIDDATA;

    header_size = AV_RB16(c->buf + 0x0C);
    chunk_count = AV_RB16(c->buf + 0x0E);
    if (header_size < 0x10 || header_size > c->buf_size)
        return AVERROR_INVALIDDATA;

    for (int i = 0; i < chunk_count; i++) {
        int64_t entry = 0x10 + (int64_t)i * 8, off;

        if (entry + 8 > header_size)
            return AVERROR_INVALIDDATA;
        off = AV_RB32(c->buf + entry);
        if (off < header_size || off + 8 > c->buf_size)
            return AVERROR_INVALIDDATA;

        if (AV_RL32(c->buf + off) == MKTAG('I','N','F','O'))
            info = off + 8;
        else if (AV_RL32(c->buf + off) == MKTAG('D','A','T','A'))
            data = off + 8;
    }
    if (!info || !data || info + 0x14 > c->buf_size)
        return AVERROR_INVALIDDATA;

    if (c->buf[info] != 0) {
        avpriv_request_sample(s, "BNS format %d", c->buf[info]);
        return AVERROR_PATCHWELCOME;
    }
    c->channels  = c->buf[info + 2];
    loop_start   = AV_RB32(c->buf + info + 0x08);
    sample_count = AV_RB32(c->buf + info + 0x0C);
    list         = info + AV_RB32(c->buf + info + 0x10);

    if (c->channels < 1 || c->channels > FF_DSP_ADPCM_MAX_CHANNELS ||
        !sample_count || sample_count > INT32_MAX / 2)
        return AVERROR_INVALIDDATA;
    if (list < 0 || list + 4LL * c->channels > c->buf_size)
        return AVERROR_INVALIDDATA;

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id    = AV_CODEC_ID_ADPCM_THP;
    st->codecpar->sample_rate = AV_RB16(c->buf + info + 0x04);
    st->codecpar->ch_layout.nb_channels = c->channels;
    if (st->codecpar->sample_rate <= 0)
        return AVERROR_INVALIDDATA;
    st->start_time = 0;
    st->duration   = sample_count;
    avpriv_set_pts_info(st, 64, 1, st->codecpar->sample_rate);

    /* The decoder reads the coefficients out of extradata and then expects
     * packets to be nothing but ADPCM frames, channel-major. */
    if ((ret = ff_alloc_extradata(st->codecpar, 32 * c->channels)) < 0)
        return ret;

    for (int ch = 0; ch < c->channels; ch++) {
        int64_t chinfo = info + AV_RB32(c->buf + list + 4 * ch);
        int64_t coefs;

        if (chinfo < 0 || chinfo + 12 > c->buf_size)
            return AVERROR_INVALIDDATA;
        c->ch_offset[ch] = data + AV_RB32(c->buf + chinfo);
        coefs            = info + AV_RB32(c->buf + chinfo + 4);
        if (coefs < 0 || coefs + 32 > c->buf_size)
            return AVERROR_INVALIDDATA;
        if (c->ch_offset[ch] < 0 ||
            c->ch_offset[ch] + ff_dsp_adpcm_byte_count(sample_count) > c->buf_size)
            return AVERROR_INVALIDDATA;
        memcpy(st->codecpar->extradata + ch * 32, c->buf + coefs, 32);
    }

    if (c->buf[info + 1] &&
        av_dict_set_int(&s->metadata, "loop_start",
                        av_rescale(loop_start, AV_TIME_BASE,
                                   st->codecpar->sample_rate), 0) < 0)
        return AVERROR(ENOMEM);

    c->samples_left = sample_count;
    return 0;
}

/* Frames per packet. Arbitrary -- the channels are contiguous, so the split is
 * ours to choose; this keeps packets at a few hundred milliseconds. */
#define BNS_FRAMES_PER_PACKET 1024

/* The decoder derives its sample count from the packet size, and a packet
 * always holds whole 14-sample frames, so the final one runs up to 13 samples
 * past the end of the stream. Tell the decoder to discard the overshoot
 * rather than letting it reach the output. */
static int bns_trim_packet(AVPacket *pkt, int64_t produced, int64_t wanted)
{
    uint8_t *side;

    if (produced <= wanted)
        return 0;
    side = av_packet_new_side_data(pkt, AV_PKT_DATA_SKIP_SAMPLES, 10);
    if (!side)
        return AVERROR(ENOMEM);
    AV_WL32(side,     0);                       /* nothing to skip at the start */
    AV_WL32(side + 4, produced - wanted);
    side[8] = side[9] = 0;
    return 0;
}

static int bns_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    BNSDemuxContext *c = s->priv_data;
    int64_t want;
    int per_ch, ret;

    if (c->samples_left <= 0)
        return AVERROR_EOF;

    want   = FFMIN(c->samples_left,
                   (int64_t)BNS_FRAMES_PER_PACKET * FF_DSP_ADPCM_SAMPLES_PER_FRAME);
    per_ch = ff_dsp_adpcm_byte_count(want);

    if ((ret = av_new_packet(pkt, per_ch * c->channels)) < 0)
        return ret;

    for (int ch = 0; ch < c->channels; ch++)
        memcpy(pkt->data + (size_t)ch * per_ch,
               c->buf + c->ch_offset[ch] + c->pos, per_ch);

    pkt->stream_index = 0;
    pkt->duration     = want;
    c->pos           += per_ch;
    c->samples_left  -= want;

    return bns_trim_packet(pkt,
                           (int64_t)per_ch / FF_DSP_ADPCM_BYTES_PER_FRAME *
                           FF_DSP_ADPCM_SAMPLES_PER_FRAME, want);
}

static int bns_read_close(AVFormatContext *s)
{
    BNSDemuxContext *c = s->priv_data;

    av_freep(&c->buf);
    return 0;
}

const FFInputFormat ff_bns_demuxer = {
    .p.name         = "bns",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Wii BNS (banner sound)"),
    .p.extensions   = "bns",
    .priv_data_size = sizeof(BNSDemuxContext),
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
    .read_probe     = bns_probe,
    .read_header    = bns_read_header,
    .read_packet    = bns_read_packet,
    .read_close     = bns_read_close,
};
