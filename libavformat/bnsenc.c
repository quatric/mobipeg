/*
 * Wii BNS (banner sound) muxer
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

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/nintendo_lz.h"
#include "libavutil/opt.h"

#include "avformat.h"
#include "avio_internal.h"
#include "bns.h"
#include "dsp_adpcm.h"
#include "internal.h"
#include "mux.h"

#define BNS_ALIGN 0x20

typedef struct BNSMuxContext {
    const AVClass *class;

    int      compress;
    int      loop;
    int64_t  loop_start;

    AVIOContext *ch_buf[FF_DSP_ADPCM_MAX_CHANNELS];
    int16_t      coefs[FF_DSP_ADPCM_MAX_CHANNELS][16];
    int          have_coefs;
    int64_t      nb_samples;
} BNSMuxContext;

static int bns_init(AVFormatContext *s)
{
    BNSMuxContext *c = s->priv_data;
    AVCodecParameters *par;

    if (s->nb_streams != 1) {
        av_log(s, AV_LOG_ERROR, "BNS carries exactly one stream\n");
        return AVERROR(EINVAL);
    }
    par = s->streams[0]->codecpar;

    if (par->codec_id != AV_CODEC_ID_ADPCM_THP) {
        av_log(s, AV_LOG_ERROR,
               "BNS carries Nintendo DSP-ADPCM only; use -c:a adpcm_thp\n");
        return AVERROR(EINVAL);
    }
    if (par->ch_layout.nb_channels > FF_DSP_ADPCM_MAX_CHANNELS)
        return AVERROR(EINVAL);
    /* The sample rate field is 16 bits wide. */
    if (par->sample_rate <= 0 || par->sample_rate > UINT16_MAX) {
        av_log(s, AV_LOG_ERROR, "sample rate must fit in 16 bits\n");
        return AVERROR(EINVAL);
    }

    for (int ch = 0; ch < par->ch_layout.nb_channels; ch++) {
        int ret = avio_open_dyn_buf(&c->ch_buf[ch]);
        if (ret < 0)
            return ret;
    }

    avpriv_set_pts_info(s->streams[0], 64, 1, par->sample_rate);
    return 0;
}

static void bns_deinit(AVFormatContext *s)
{
    BNSMuxContext *c = s->priv_data;

    for (int ch = 0; ch < FF_DSP_ADPCM_MAX_CHANNELS; ch++)
        ffio_free_dyn_buf(&c->ch_buf[ch]);
}

static int bns_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    BNSMuxContext *c = s->priv_data;
    int channels = s->streams[0]->codecpar->ch_layout.nb_channels;
    size_t side_size;
    const uint8_t *side;
    int per_ch;

    side = av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, &side_size);
    if (!side) {
        side      = s->streams[0]->codecpar->extradata;
        side_size = s->streams[0]->codecpar->extradata_size;
    }
    if (side && side_size >= 32 * (size_t)channels) {
        for (int ch = 0; ch < channels; ch++)
            for (int i = 0; i < 16; i++)
                c->coefs[ch][i] = (int16_t)AV_RB16(side + ch * 32 + i * 2);
        c->have_coefs = 1;
    }

    if (pkt->size % (channels * FF_DSP_ADPCM_BYTES_PER_FRAME)) {
        av_log(s, AV_LOG_ERROR, "packet is not a whole number of frames\n");
        return AVERROR_INVALIDDATA;
    }
    per_ch = pkt->size / channels;

    for (int ch = 0; ch < channels; ch++)
        avio_write(c->ch_buf[ch], pkt->data + (size_t)ch * per_ch, per_ch);

    if (pkt->duration > 0)
        c->nb_samples += pkt->duration;
    else
        c->nb_samples += (int64_t)per_ch / FF_DSP_ADPCM_BYTES_PER_FRAME *
                         FF_DSP_ADPCM_SAMPLES_PER_FRAME;

    return 0;
}

/* Build the whole file in memory. It has to be assembled before it can be
 * written whatever happens -- the header states the total size and the chunk
 * offsets -- and having it in one buffer is also what lets -compress run over
 * the finished file. */
static int bns_build(AVFormatContext *s, uint8_t *const *data, const int *size,
                     uint8_t **outp, int *out_sizep)
{
    BNSMuxContext *c = s->priv_data;
    AVCodecParameters *par = s->streams[0]->codecpar;
    int channels = par->ch_layout.nb_channels;
    AVIOContext *pb = NULL;
    /* INFO body: fixed fields, then the channel-offset list, the per-channel
     * info records, and the DSP blocks. Offsets in the list are relative to
     * the body's first byte; the ones in a record point into it too, except
     * the data offset, which is relative to the DATA body. */
    int list_rel   = 0x14;
    int chinfo_rel = list_rel + 4 * channels;
    int dsp_rel    = chinfo_rel + 12 * channels;
    int info_body  = dsp_rel + BNS_CHANNEL_DSP_SIZE * channels;
    int info_size  = FFALIGN(8 + info_body, BNS_ALIGN);
    int data_body  = 0;
    int info_off   = 0x20;
    int data_off, file_size, ret;

    for (int ch = 0; ch < channels; ch++)
        data_body += FFALIGN(size[ch], BNS_ALIGN);
    data_off  = info_off + info_size;
    file_size = data_off + FFALIGN(8 + data_body, BNS_ALIGN);

    if ((ret = avio_open_dyn_buf(&pb)) < 0)
        return ret;

    ffio_wfourcc(pb, "BNS ");
    avio_wb32(pb, BNS_VERSION);
    avio_wb32(pb, file_size);
    avio_wb16(pb, 0x20);                        /* header size: covers the chunk table */
    avio_wb16(pb, 2);                           /* chunk count */
    avio_wb32(pb, info_off);
    avio_wb32(pb, info_size);
    avio_wb32(pb, data_off);
    avio_wb32(pb, file_size - data_off);
    ffio_fill(pb, 0, info_off - 0x20);

    ffio_wfourcc(pb, "INFO");
    avio_wb32(pb, info_size);
    avio_w8(pb, 0);                             /* format: DSP-ADPCM */
    avio_w8(pb, c->loop);
    avio_w8(pb, channels);
    avio_w8(pb, 0);
    avio_wb16(pb, par->sample_rate);
    avio_wb16(pb, 0);
    avio_wb32(pb, c->loop ? c->loop_start : 0);
    avio_wb32(pb, c->nb_samples);
    avio_wb32(pb, list_rel);

    for (int ch = 0; ch < channels; ch++)
        avio_wb32(pb, chinfo_rel + 12 * ch);

    for (int ch = 0, off = 0; ch < channels; ch++) {
        avio_wb32(pb, off);                     /* into the DATA body */
        avio_wb32(pb, dsp_rel + BNS_CHANNEL_DSP_SIZE * ch);
        avio_wb32(pb, 0);
        off += FFALIGN(size[ch], BNS_ALIGN);
    }

    for (int ch = 0; ch < channels; ch++) {
        int64_t loop_frames = c->loop ? c->loop_start /
                                        FF_DSP_ADPCM_SAMPLES_PER_FRAME : 0;
        int64_t loop_off = loop_frames * FF_DSP_ADPCM_BYTES_PER_FRAME;
        int16_t loop_h1 = 0, loop_h2 = 0;

        if (loop_off > 0 && loop_off < size[ch])
            ff_dsp_adpcm_advance(data[ch], loop_frames, c->coefs[ch],
                                 &loop_h1, &loop_h2);

        for (int i = 0; i < 16; i++)
            avio_wb16(pb, c->coefs[ch][i]);
        avio_wb16(pb, 0);                       /* gain */
        avio_wb16(pb, size[ch] ? data[ch][0] : 0);   /* initial predictor/scale */
        avio_wb16(pb, 0);                       /* initial hist1 */
        avio_wb16(pb, 0);                       /* initial hist2 */
        avio_wb16(pb, loop_off < size[ch] ? data[ch][loop_off] : 0);
        avio_wb16(pb, loop_h1);
        avio_wb16(pb, loop_h2);
    }
    ffio_fill(pb, 0, info_size - (8 + info_body));

    ffio_wfourcc(pb, "DATA");
    avio_wb32(pb, file_size - data_off);
    /* Channels are stored one after another, not interleaved; the channel
     * records above are what says where each one starts. */
    for (int ch = 0; ch < channels; ch++) {
        avio_write(pb, data[ch], size[ch]);
        ffio_fill(pb, 0, FFALIGN(size[ch], BNS_ALIGN) - size[ch]);
    }
    ffio_fill(pb, 0, file_size - data_off - (8 + data_body));

    *out_sizep = avio_close_dyn_buf(pb, outp);
    return *out_sizep < 0 ? *out_sizep : 0;
}

static int bns_write_trailer(AVFormatContext *s)
{
    BNSMuxContext *c = s->priv_data;
    int channels = s->streams[0]->codecpar->ch_layout.nb_channels;
    uint8_t *data[FF_DSP_ADPCM_MAX_CHANNELS] = { NULL };
    int size[FF_DSP_ADPCM_MAX_CHANNELS] = { 0 };
    uint8_t *file = NULL, *packed = NULL;
    int file_size = 0, ret = 0;

    for (int ch = 0; ch < channels; ch++)
        size[ch] = avio_close_dyn_buf(c->ch_buf[ch], &data[ch]);
    memset(c->ch_buf, 0, sizeof(c->ch_buf));

    for (int ch = 0; ch < channels; ch++) {
        if (size[ch] < 0) {
            ret = size[ch];
            goto end;
        }
    }
    if (!c->nb_samples) {
        av_log(s, AV_LOG_ERROR, "no audio was written\n");
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    if (!c->have_coefs) {
        av_log(s, AV_LOG_ERROR, "no DSP-ADPCM coefficient table available; "
               "the input has to come from the adpcm_thp encoder or from a "
               "container that carries the table\n");
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    if ((ret = bns_build(s, data, size, &file, &file_size)) < 0)
        goto end;

    if (c->compress) {
        int cap = avpriv_nintendo_lz10_bound(file_size);
        int n;

        packed = av_malloc(cap);
        if (!packed) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        n = avpriv_nintendo_lz10_compress(file, file_size, packed, cap);
        if (n < 0) {
            ret = n;
            goto end;
        }
        /* Compression that grew the file would be a strange thing to ship,
         * and the demuxer reads either form, so keep whichever is smaller. */
        if (n < file_size) {
            avio_write(s->pb, packed, n);
            goto end;
        }
        av_log(s, AV_LOG_INFO,
               "LZ10 did not shrink this stream; storing it uncompressed\n");
    }
    avio_write(s->pb, file, file_size);

end:
    av_freep(&packed);
    av_freep(&file);
    for (int ch = 0; ch < channels; ch++)
        av_free(data[ch]);
    return ret;
}

#define OFFSET(x) offsetof(BNSMuxContext, x)
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption bns_options[] = {
    { "compress", "wrap the file in Nintendo LZ10, as banner archives do",
      OFFSET(compress), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, E },
    { "loop", "mark the stream as looping", OFFSET(loop), AV_OPT_TYPE_BOOL,
      { .i64 = 0 }, 0, 1, E },
    { "loop_start", "loop start, in samples", OFFSET(loop_start),
      AV_OPT_TYPE_INT64, { .i64 = 0 }, 0, INT32_MAX, E },
    { NULL },
};

static const AVClass bns_muxer_class = {
    .class_name = "bns muxer",
    .item_name  = av_default_item_name,
    .option     = bns_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFOutputFormat ff_bns_muxer = {
    .p.name           = "bns",
    .p.long_name      = NULL_IF_CONFIG_SMALL("Wii BNS (banner sound)"),
    .p.extensions     = "bns",
    .p.audio_codec    = AV_CODEC_ID_ADPCM_THP,
    .p.video_codec    = AV_CODEC_ID_NONE,
    .p.subtitle_codec = AV_CODEC_ID_NONE,
    .p.flags          = AVFMT_NOTIMESTAMPS,
    .p.priv_class     = &bns_muxer_class,
    .priv_data_size   = sizeof(BNSMuxContext),
    .init             = bns_init,
    .deinit           = bns_deinit,
    .write_packet     = bns_write_packet,
    .write_trailer    = bns_write_trailer,
    .flags_internal   = FF_OFMT_FLAG_MAX_ONE_OF_EACH |
                        FF_OFMT_FLAG_ONLY_DEFAULT_CODECS,
};
