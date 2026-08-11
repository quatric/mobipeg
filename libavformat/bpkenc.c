/*
 * BPK1 muxer
 * Copyright (c) 2026
 *
 * Swapdoodle / Swapnote chunk archive writer.
 */
#include "config.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/internal.h"
#include "libavutil/mem.h"
#include "avformat.h"
#include "internal.h"
#include "mux.h"

typedef struct BPKMuxContext {
    uint32_t *offset, *size;
    unsigned *written;
    int header_size;
} BPKMuxContext;

static int bpk_write_header(AVFormatContext *s)
{
    BPKMuxContext *c = s->priv_data;
    int i;

    if (!(s->pb->seekable & AVIO_SEEKABLE_NORMAL) || !s->nb_streams)
        return AVERROR(ESPIPE);

    c->header_size = FFALIGN(0x40 + s->nb_streams * 20, 16);
    c->offset = av_calloc(s->nb_streams, sizeof(*c->offset));
    c->size = av_calloc(s->nb_streams, sizeof(*c->size));
    c->written = av_calloc(s->nb_streams, sizeof(*c->written));
    if (!c->offset || !c->size || !c->written)
        return AVERROR(ENOMEM);

    avio_wl32(s->pb, MKTAG('B', 'P', 'K', '1'));
    avio_wl32(s->pb, s->nb_streams);
    avio_wl32(s->pb, 7);
    avio_wl32(s->pb, 0);
    avio_wl32(s->pb, c->header_size);
    for (i = 20; i < c->header_size; i++)
        avio_w8(s->pb, 0);
    return 0;
}

static int bpk_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    BPKMuxContext *c = s->priv_data;
    int i = pkt->stream_index;

    if ((unsigned)i >= s->nb_streams || c->written[i]++)
        return AVERROR(EINVAL);
    c->offset[i] = avio_tell(s->pb);
    c->size[i] = pkt->size;
    avio_write(s->pb, pkt->data, pkt->size);
    while (avio_tell(s->pb) & 3)
        avio_w8(s->pb, 0);
    return 0;
}

static int bpk_write_trailer(AVFormatContext *s)
{
    BPKMuxContext *c = s->priv_data;
    int64_t end = avio_tell(s->pb);
    unsigned i;

    avio_seek(s->pb, 12, SEEK_SET);
    avio_wl32(s->pb, end);
    avio_seek(s->pb, 0x40, SEEK_SET);
    for (i = 0; i < s->nb_streams; i++) {
        AVDictionaryEntry *entry = av_dict_get(s->streams[i]->metadata,
                                               "title", NULL, 0);
        const char *name = entry ? entry->value : "DATA";
        uint8_t name_field[8] = { 0 };

        memcpy(name_field, name, FFMIN(strlen(name), sizeof(name_field)));
        avio_wl32(s->pb, c->offset[i]);
        avio_wl32(s->pb, c->size[i]);
        avio_wl32(s->pb, 0); /* Hash algorithm is not standard CRC32. */
        avio_write(s->pb, name_field, sizeof(name_field));
    }
    avio_seek(s->pb, end, SEEK_SET);
    av_freep(&c->offset);
    av_freep(&c->size);
    av_freep(&c->written);
    return 0;
}

const FFOutputFormat ff_bpk_muxer = {
    .p.name            = "bpk",
    .p.long_name       = NULL_IF_CONFIG_SMALL("Swapdoodle BPK1"),
    .p.extensions      = "bpk,bpk1",
    .p.audio_codec     = AV_CODEC_ID_NONE,
    .p.video_codec     = AV_CODEC_ID_MJPEG,
    .p.subtitle_codec  = AV_CODEC_ID_NONE,
    .priv_data_size    = sizeof(BPKMuxContext),
    .write_header      = bpk_write_header,
    .write_packet      = bpk_write_packet,
    .write_trailer     = bpk_write_trailer,
    .p.flags           = AVFMT_NOTIMESTAMPS,
};
