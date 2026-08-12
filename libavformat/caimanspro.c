/* Caimans Pro GBA Video ROM demuxer */
#include <limits.h>
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "avformat.h"
#include "demux.h"
#include "internal.h"

#define TABLE_A 0xc4b0
#define TABLE_B 0xc610
#define DATA    0xc768
#define ENTRIES 86
#define IWRAM_OFFSET 0x4f6c
#define IWRAM_SIZE   (0xbfbc - IWRAM_OFFSET)

/* The flat ADPCM stream, immediately after the video's terminal record and
 * running to the end of the ROM. Nothing indexes it: the player just walks a
 * cursor from here, so both the start and the codec state are stream-global.
 * The rate is the audio timer's own: TM0 reloads 0xf7b0, i.e. a period of
 * 2128 cycles, so 16777216 / 2128 = 7884.03 Hz. Read off a live TM0 and
 * cross-checked against the audio cursor's advance over a whole pass
 * (377,344 bytes over 1149 frame ticks = 7883.9 Hz). */
#define AUDIO_OFFSET 0x1473e1
#define AUDIO_PERIOD 2128
#define AUDIO_BLOCK  512            /* bytes per emitted packet */

typedef struct CaimansProDemuxContext {
    uint32_t off[ENTRIES + 1], pts[ENTRIES];
    int index, stream, audio_stream;
    int64_t file_size, audio_pos;
} CaimansProDemuxContext;

static int caimanspro_probe(const AVProbeData *p)
{
    if (p->buf_size < DATA + 4 || AV_RL32(p->buf + TABLE_A) != 16 ||
        AV_RL32(p->buf + TABLE_B) != 60 ||
        AV_RB24(p->buf + DATA + 16) >> 2 != 0x20)
        return 0;
    return AVPROBE_SCORE_MAX;
}

static int caimanspro_read_header(AVFormatContext *avctx)
{
    CaimansProDemuxContext *s = avctx->priv_data;
    AVStream *st;
    if (avio_size(avctx->pb) <= DATA || avio_seek(avctx->pb, TABLE_A, SEEK_SET) < 0)
        return AVERROR_INVALIDDATA;
    for (int i = 0; i <= ENTRIES; i++) {
        s->off[i] = avio_rl32(avctx->pb);
        if (!s->off[i] || (i && s->off[i] <= s->off[i - 1]))
            return AVERROR_INVALIDDATA;
    }
    if (avio_seek(avctx->pb, TABLE_B, SEEK_SET) < 0)
        return AVERROR_INVALIDDATA;
    for (int i = 0; i < ENTRIES; i++)
        s->pts[i] = avio_rl32(avctx->pb);
    st = avformat_new_stream(avctx, NULL);
    if (!st) return AVERROR(ENOMEM);
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = AV_CODEC_ID_CAIMANSPRO;
    st->codecpar->width = 240; st->codecpar->height = 160;
    if (avio_size(avctx->pb) > INT_MAX - IWRAM_SIZE)
        return AVERROR_INVALIDDATA;
    st->codecpar->extradata = av_mallocz(IWRAM_SIZE + avio_size(avctx->pb) + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!st->codecpar->extradata ||
        avio_seek(avctx->pb, IWRAM_OFFSET, SEEK_SET) < 0 ||
        avio_read(avctx->pb, st->codecpar->extradata, IWRAM_SIZE) != IWRAM_SIZE ||
        avio_seek(avctx->pb, 0, SEEK_SET) < 0 ||
        avio_read(avctx->pb, st->codecpar->extradata + IWRAM_SIZE, avio_size(avctx->pb)) != avio_size(avctx->pb))
        return AVERROR_INVALIDDATA;
    st->codecpar->extradata_size = IWRAM_SIZE + avio_size(avctx->pb);
    s->file_size = avio_size(avctx->pb);
    avpriv_set_pts_info(st, 64, 256 * 0x1555, 16777216);
    s->stream = st->index;

    if (s->file_size > AUDIO_OFFSET) {
        AVStream *ast = avformat_new_stream(avctx, NULL);
        if (!ast) return AVERROR(ENOMEM);
        ast->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
        /* The same kernel as Caimans 2.2's, proven byte-identical. */
        ast->codecpar->codec_id    = AV_CODEC_ID_CAIMANS22_AUDIO;
        ast->codecpar->ch_layout   = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
        ast->codecpar->sample_rate = 16777216 / AUDIO_PERIOD;
        ast->codecpar->bits_per_coded_sample = 4;
        ast->codecpar->block_align = 32;
        avpriv_set_pts_info(ast, 64, AUDIO_PERIOD, 16777216);
        s->audio_stream = ast->index;
        s->audio_pos = AUDIO_OFFSET;
    } else {
        s->audio_stream = -1;
    }
    return 0;
}

static int caimanspro_read_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    CaimansProDemuxContext *s = avctx->priv_data;
    int64_t pos;
    int n, ret;

    /* Video and audio are two separate flat runs in the ROM with no
     * interleaving of their own, so interleave them here by comparing the
     * next packet of each in a common 1/16777216 s unit. */
    if (s->audio_stream >= 0 && s->audio_pos < s->file_size) {
        int64_t apts = (s->audio_pos - AUDIO_OFFSET) * 2;
        int64_t atime = apts * AUDIO_PERIOD;
        int64_t vtime = s->index > ENTRIES ? INT64_MAX
                      : (int64_t)(s->index ? s->pts[s->index - 1] : 0) * 256 * 0x1555;
        if (atime <= vtime) {
            n = FFMIN(AUDIO_BLOCK, s->file_size - s->audio_pos);
            if (avio_seek(avctx->pb, s->audio_pos, SEEK_SET) < 0)
                return AVERROR_INVALIDDATA;
            ret = av_get_packet(avctx->pb, pkt, n);
            if (ret != n) return ret < 0 ? ret : AVERROR_EOF;
            pkt->stream_index = s->audio_stream;
            pkt->pts = pkt->dts = apts;
            pkt->duration = n * 2;
            pkt->pos = s->audio_pos;
            pkt->flags |= AV_PKT_FLAG_KEY;
            s->audio_pos += n;
            return 0;
        }
    }

    if (s->index > ENTRIES) return AVERROR_EOF;
    pos = DATA + s->off[s->index];
    if (s->index < ENTRIES) {
        n = s->off[s->index + 1] - s->off[s->index];
    } else {
        /* The seek table's last entry is not the end of the picture stream:
         * a few more pictures follow it, ending at a terminal record the
         * player refuses and then at the flat audio. Their extent is not in
         * any recovered table, so hand over the rest of the ROM and let the
         * decoder walk it -- it stops on the terminal record. */
        n = FFMIN((s->audio_stream >= 0 ? AUDIO_OFFSET : s->file_size) - pos, INT_MAX);
        if (n <= 0) return AVERROR_EOF;
    }
    if (n <= 0 || avio_seek(avctx->pb, pos, SEEK_SET) < 0)
        return AVERROR_INVALIDDATA;
    ret = av_get_packet(avctx->pb, pkt, n);
    if (ret != n) return ret < 0 ? ret : AVERROR_EOF;
    pkt->stream_index = s->stream;
    pkt->pts = pkt->dts = s->index ? s->pts[s->index - 1] : 0;
    pkt->pos = pos;
    pkt->flags |= AV_PKT_FLAG_KEY;
    s->index++;
    return 0;
}

const FFInputFormat ff_caimanspro_demuxer = {
    .p.name = "caimanspro", .p.long_name = "Caimans Pro GBA Video ROM",
    .p.extensions = "gba", .priv_data_size = sizeof(CaimansProDemuxContext),
    .read_probe = caimanspro_probe, .read_header = caimanspro_read_header,
    .read_packet = caimanspro_read_packet,
};
