/*
 * BPK1 demuxer
 * Copyright (c) 2026
 *
 * Swapdoodle / Swapnote (Nintendo 3DS) note container.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/nintendo_lz.h"
#include "libavcodec/avcodec.h"
#include "avformat.h"
#include "demux.h"
#include "internal.h"

#define BPK_HEADER_SIZE 0x40
#define BPK_ENTRY_SIZE  20

typedef struct BPKEntry {
    uint32_t offset;
    uint32_t size;
    char name[9];
    int stream_index;
} BPKEntry;

typedef struct BPKDemuxContext {
    BPKEntry *entries;
    uint8_t *data;
    int data_size;
    unsigned nb_entries;
    unsigned next_entry;
    int note_stream_index;
    unsigned next_note_entry;
    unsigned note_frame_number;
} BPKDemuxContext;

static int bpk_probe(const AVProbeData *p)
{
    uint32_t count, total, header_size;

    if (p->buf_size < 4)
        return 0;
    if (p->buf[0] == AV_NINTENDO_LZ10_TAG)
        return AVPROBE_SCORE_EXTENSION;
    if (p->buf_size < BPK_HEADER_SIZE || AV_RL32(p->buf) != MKTAG('B', 'P', 'K', '1'))
        return 0;
    count = AV_RL32(p->buf + 4);
    total = AV_RL32(p->buf + 12);
    header_size = AV_RL32(p->buf + 16);
    if (count > (UINT32_MAX - BPK_HEADER_SIZE) / BPK_ENTRY_SIZE ||
        header_size < BPK_HEADER_SIZE + count * BPK_ENTRY_SIZE || total < header_size)
        return AVPROBE_SCORE_RETRY;
    return AVPROBE_SCORE_MAX;
}

static av_cold int bpk_read_header(AVFormatContext *s)
{
    BPKDemuxContext *c = s->priv_data;
    AVIOContext *pb = s->pb;
    uint32_t count, total, header_size;
    int64_t file_size = avio_size(pb);
    uint8_t *data;
    unsigned i;
    int ret;

    if (file_size <= 0 || file_size > INT_MAX)
        return AVERROR_INVALIDDATA;
    data = av_malloc(file_size);
    if (!data)
        return AVERROR(ENOMEM);
    if (avio_seek(pb, 0, SEEK_SET) < 0 || avio_read(pb, data, file_size) != file_size) {
        av_free(data);
        return AVERROR_INVALIDDATA;
    }
    if (data[0] == AV_NINTENDO_LZ10_TAG) {
        int out_size = avpriv_nintendo_lz10_size(data, file_size);
        uint8_t *out;
        if (out_size < BPK_HEADER_SIZE) {
            av_free(data);
            return AVERROR_INVALIDDATA;
        }
        out = av_malloc(out_size);
        if (!out) {
            av_free(data);
            return AVERROR(ENOMEM);
        }
        ret = avpriv_nintendo_lz10_decompress(data, file_size, out, out_size);
        av_free(data);
        if (ret != out_size) {
            av_free(out);
            return AVERROR_INVALIDDATA;
        }
        data = out;
        file_size = out_size;
    }
    c->data = data;
    c->data_size = file_size;
    if (AV_RL32(data) != MKTAG('B', 'P', 'K', '1'))
        return AVERROR_INVALIDDATA;
    count = AV_RL32(data + 4);
    total = AV_RL32(data + 12);
    header_size = AV_RL32(data + 16);
    if (count > (UINT32_MAX - BPK_HEADER_SIZE) / BPK_ENTRY_SIZE ||
        header_size < BPK_HEADER_SIZE + count * BPK_ENTRY_SIZE || total < header_size ||
        total > file_size)
        return AVERROR_INVALIDDATA;

    c->entries = av_calloc(count, sizeof(*c->entries));
    if (!c->entries && count)
        return AVERROR(ENOMEM);
    c->nb_entries = count;

    /* The first stream is the complete archive.  Unlike a standalone
     * SHEET1 section it gives the Swapdoodle decoder access to the palette,
     * stationery, badges, icons and sender Mii needed for a real page. */
    {
        AVStream *st = avformat_new_stream(s, NULL);
        if (!st)
            return AVERROR(ENOMEM);
        c->note_stream_index = st->index;
        st->start_time = 0;
        st->duration = 1;
        avpriv_set_pts_info(st, 64, 1, 1);
        st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        st->codecpar->codec_id = AV_CODEC_ID_SWAPDOODLE;
        st->codecpar->width = st->codecpar->height = 256;
        st->codecpar->extradata = av_mallocz(file_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!st->codecpar->extradata)
            return AVERROR(ENOMEM);
        memcpy(st->codecpar->extradata, data, file_size);
        st->codecpar->extradata_size = file_size;
        av_dict_set(&st->metadata, "title", "Swapdoodle note", 0);
    }

    for (i = 0; i < count; i++) {
        BPKEntry *entry = &c->entries[i];
        const uint8_t *dir = data + BPK_HEADER_SIZE + i * BPK_ENTRY_SIZE;
        entry->offset = AV_RL32(dir);
        entry->size = AV_RL32(dir + 4);
        memcpy(entry->name, dir + 12, 8);
        if (entry->offset < header_size || entry->offset > total ||
            entry->size > total - entry->offset)
            return AVERROR_INVALIDDATA;

    }
    for (i = 0; i < count; i++) {
        BPKEntry *entry = &c->entries[i];
        AVStream *st;
        uint8_t first[2];

        st = avformat_new_stream(s, NULL);
        if (!st)
            return AVERROR(ENOMEM);
        entry->stream_index = st->index;
        st->start_time = 0;
        st->duration = 1;
        avpriv_set_pts_info(st, 64, 1, 1);
        av_dict_set(&st->metadata, "title", entry->name[0] ? entry->name : "unnamed", 0);
        av_dict_set_int(&st->metadata, "bpk_offset", entry->offset, 0);
        av_dict_set_int(&st->metadata, "bpk_size", entry->size, 0);

        /* JPEG chunks (PHOTO1, STBARD1, and thumbnails) stay byte-identical
         * in packets while becoming directly decodable by the MJPEG decoder. */
        first[0] = data[entry->offset];
        first[1] = entry->size > 1 ? data[entry->offset + 1] : 0;
        if (!strcmp(entry->name, "SHEET1")) {
            st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
            st->codecpar->codec_id = AV_CODEC_ID_SWAPDOODLE;
            st->codecpar->width = st->codecpar->height = 256;
        } else if (entry->size >= 2 && first[0] == 0xff && first[1] == 0xd8) {
            st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
            st->codecpar->codec_id = AV_CODEC_ID_MJPEG;
        } else {
            st->codecpar->codec_type = AVMEDIA_TYPE_DATA;
            st->codecpar->codec_id = AV_CODEC_ID_BIN_DATA;
        }
    }
    return 0;
}

static int bpk_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    BPKDemuxContext *c = s->priv_data;
    BPKEntry *entry;
    int ret;

    while (c->next_note_entry < c->nb_entries) {
        BPKEntry *note = &c->entries[c->next_note_entry++];
        int ret;
        if (strcmp(note->name, "SHEET1"))
            continue;
        if ((ret = av_new_packet(pkt, note->size)) < 0)
            return ret;
        memcpy(pkt->data, c->data + note->offset, note->size);
        pkt->stream_index = c->note_stream_index;
        pkt->pts = pkt->dts = c->note_frame_number++;
        pkt->duration = 1;
        return 0;
    }
    if (c->next_entry >= c->nb_entries)
        return AVERROR_EOF;
    entry = &c->entries[c->next_entry++];
    if ((ret = av_new_packet(pkt, entry->size)) < 0)
        return ret;
    memcpy(pkt->data, c->data + entry->offset, entry->size);
    pkt->stream_index = entry->stream_index;
    pkt->pts = pkt->dts = 0;
    pkt->duration = 1;
    pkt->pos = entry->offset;
    return 0;
}

static av_cold int bpk_read_close(AVFormatContext *s)
{
    BPKDemuxContext *c = s->priv_data;
    av_freep(&c->entries);
    av_freep(&c->data);
    return 0;
}

const FFInputFormat ff_bpk_demuxer = {
    .p.name         = "bpk",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Swapdoodle BPK1"),
    .p.extensions   = "bpk,bpk1,apd",
    .priv_data_size  = sizeof(BPKDemuxContext),
    .read_probe      = bpk_probe,
    .read_header     = bpk_read_header,
    .read_packet     = bpk_read_packet,
    .read_close      = bpk_read_close,
};
