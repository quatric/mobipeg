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

typedef struct CaimansProDemuxContext {
    uint32_t off[ENTRIES + 1], pts[ENTRIES];
    int index, stream;
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
    avpriv_set_pts_info(st, 64, 256 * 0x1555, 16777216);
    s->stream = st->index;
    return 0;
}

static int caimanspro_read_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    CaimansProDemuxContext *s = avctx->priv_data;
    int n, ret;
    if (s->index >= ENTRIES) return AVERROR_EOF;
    n = s->off[s->index + 1] - s->off[s->index];
    if (n <= 0 || avio_seek(avctx->pb, DATA + s->off[s->index], SEEK_SET) < 0)
        return AVERROR_INVALIDDATA;
    ret = av_get_packet(avctx->pb, pkt, n);
    if (ret != n) return ret < 0 ? ret : AVERROR_EOF;
    pkt->stream_index = s->stream;
    pkt->pts = pkt->dts = s->index ? s->pts[s->index - 1] : 0;
    pkt->pos = DATA + s->off[s->index++];
    return 0;
}

const FFInputFormat ff_caimanspro_demuxer = {
    .p.name = "caimanspro", .p.long_name = "Caimans Pro GBA Video ROM",
    .p.extensions = "gba", .priv_data_size = sizeof(CaimansProDemuxContext),
    .read_probe = caimanspro_probe, .read_header = caimanspro_read_header,
    .read_packet = caimanspro_read_packet,
};
