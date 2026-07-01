/*
 * Copyright (c) 2026 quatric - quatricsoftware@gmail.com
 * Shared in-muxer audio encoder for MobiClip containers (MOFLEX / MODS).
 *
 * The .mo muxer (moenc.c) accepts PCM and encodes audio to the target MobiClip
 * audio codec inside the muxer, so `-mo_audio fastaudio|adpcm|pcm` works
 * regardless of which encoder fftools would otherwise pick.  This header
 * provides the same capability for the MOFLEX and MODS muxers, which carry
 * adpcm_ima_moflex / fastaudio / pcm_s16le audio.
 *
 * Usage: init once (target codec from -mo_audio), feed interleaved S16 PCM via
 * send_pcm(), drain encoded packets via recv() until AVERROR(EAGAIN), and at
 * end-of-stream call flush() then drain again.  PCM passthrough (target==2)
 * returns the buffered samples as a single packet on flush or per send.
 *
 * This file is part of FFmpeg and is licensed LGPL 2.1+ like the rest.
 */
#ifndef AVFORMAT_MO_AUDIOENC_H
#define AVFORMAT_MO_AUDIOENC_H

#include "libavcodec/avcodec.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/channel_layout.h"

enum { MOBI_AUDIO_FASTAUDIO = 0, MOBI_AUDIO_ADPCM = 1, MOBI_AUDIO_PCM = 2 };

typedef struct MobiAudioEnc {
    int target;                 /* MOBI_AUDIO_* */
    int channels;
    AVCodecContext *enc;        /* NULL for PCM passthrough */
    AVFrame *frame;             /* planar S16P scratch frame for the encoder */
    int frame_size;             /* samples per channel per encoder frame */
    int16_t *buf;               /* buffered interleaved S16 PCM */
    int nsamp;                  /* buffered sample-frames (per channel) */
    int cap;                    /* capacity in sample-frames */
    int draining;               /* set after flush() */
} MobiAudioEnc;

/* fastaudio_ds_mods: set 1 only for the DS .mods container, whose FastAudio
 * decode folds the shared low bit into each segment's last sample.  3DS .moflex
 * must pass 0 so its (already hardware-correct) bitstream stays unchanged. */
static int mobi_aenc_init(MobiAudioEnc *a, int target, enum AVCodecID adpcm_id,
                          const AVChannelLayout *chl, int sample_rate,
                          int fastaudio_ds_mods)
{
    memset(a, 0, sizeof(*a));
    a->target   = target;
    a->channels = chl->nb_channels > 0 ? chl->nb_channels : 1;

    if (target == MOBI_AUDIO_PCM) {
        a->frame_size = 256;
        return 0;
    }

    enum AVCodecID id = (target == MOBI_AUDIO_FASTAUDIO) ? AV_CODEC_ID_FASTAUDIO
                                                         : adpcm_id;
    const AVCodec *codec = avcodec_find_encoder(id);
    if (!codec)
        return AVERROR_ENCODER_NOT_FOUND;

    /* fastaudio wants float planar; the IMA/MOFLEX ADPCM encoders want S16P. */
    enum AVSampleFormat sfmt = (target == MOBI_AUDIO_FASTAUDIO)
                             ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_S16P;

    a->enc = avcodec_alloc_context3(codec);
    if (!a->enc)
        return AVERROR(ENOMEM);
    av_channel_layout_copy(&a->enc->ch_layout, chl);
    a->enc->sample_rate = sample_rate;
    a->enc->sample_fmt  = sfmt;
    if (target == MOBI_AUDIO_FASTAUDIO) {
        a->enc->frame_size = 256;
        if (fastaudio_ds_mods && a->enc->priv_data)
            av_opt_set_int(a->enc->priv_data, "ds_mods", 1, 0);
    }
    if (avcodec_open2(a->enc, codec, NULL) < 0) {
        avcodec_free_context(&a->enc);
        return AVERROR(EINVAL);
    }
    a->frame_size = a->enc->frame_size > 0 ? a->enc->frame_size : 256;

    a->frame = av_frame_alloc();
    if (!a->frame) {
        avcodec_free_context(&a->enc);
        return AVERROR(ENOMEM);
    }
    a->frame->format = sfmt;
    a->frame->nb_samples = a->frame_size;
    av_channel_layout_copy(&a->frame->ch_layout, chl);
    if (av_frame_get_buffer(a->frame, 0) < 0) {
        av_frame_free(&a->frame);
        avcodec_free_context(&a->enc);
        return AVERROR(ENOMEM);
    }
    return 0;
}

static int mobi_aenc_append(MobiAudioEnc *a, const int16_t *inter, int nb_per_ch)
{
    int need = a->nsamp + nb_per_ch;
    if (need > a->cap) {
        int ncap = a->cap ? a->cap * 2 : 4096;
        while (ncap < need) ncap *= 2;
        int16_t *nb = av_realloc(a->buf, (size_t)ncap * a->channels * sizeof(int16_t));
        if (!nb) return AVERROR(ENOMEM);
        a->buf = nb; a->cap = ncap;
    }
    memcpy(a->buf + a->nsamp * a->channels, inter,
           (size_t)nb_per_ch * a->channels * sizeof(int16_t));
    a->nsamp += nb_per_ch;
    return 0;
}

/* Buffer interleaved S16 PCM (nb_per_ch sample-frames). */
static int mobi_aenc_send_pcm(MobiAudioEnc *a, const int16_t *inter, int nb_per_ch)
{
    return mobi_aenc_append(a, inter, nb_per_ch);
}

/* Mark end of input; subsequent recv() will drain partial/last frame. */
static void mobi_aenc_flush(MobiAudioEnc *a)
{
    a->draining = 1;
}

/* Consume `consume` sample-frames from the front of the PCM buffer. */
static void mobi_aenc_consume(MobiAudioEnc *a, int consume)
{
    if (consume >= a->nsamp) { a->nsamp = 0; return; }
    memmove(a->buf, a->buf + consume * a->channels,
            (size_t)(a->nsamp - consume) * a->channels * sizeof(int16_t));
    a->nsamp -= consume;
}

/*
 * Produce the next encoded audio packet into *out (caller owns out->data via
 * the normal AVPacket lifecycle; for PCM passthrough the data is freshly
 * allocated and out must be unref'd by the caller).
 * Returns 0 on success, AVERROR(EAGAIN) when more input is needed, or <0 error.
 */
static int mobi_aenc_recv(MobiAudioEnc *a, AVPacket *out)
{
    if (a->target == MOBI_AUDIO_PCM) {
        if (a->nsamp == 0)
            return AVERROR(EAGAIN);
        /* Emit everything buffered when draining; otherwise emit in fixed size frames
         * (if frame_size is set) or modest chunks so packets aren't unbounded. */
        int frames = a->draining ? a->nsamp : (a->frame_size > 0 ? (a->nsamp >= a->frame_size ? a->frame_size : 0) : (a->nsamp >= 1024 ? a->nsamp : 0));
        if (frames == 0)
            return AVERROR(EAGAIN);
        int bytes = frames * a->channels * (int)sizeof(int16_t);
        if (av_new_packet(out, bytes) < 0)
            return AVERROR(ENOMEM);
        memcpy(out->data, a->buf, bytes);
        mobi_aenc_consume(a, frames);
        return 0;
    }

    /* Encoder path: try to receive first (handles drain), else feed a frame. */
    for (;;) {
        int ret = avcodec_receive_packet(a->enc, out);
        if (ret == 0 || ret != AVERROR(EAGAIN))
            return ret; /* 0=packet, AVERROR_EOF or error returned as-is */

        if (a->nsamp >= a->frame_size) {
            /* Deinterleave a full frame into the planar scratch frame. */
            if (av_frame_make_writable(a->frame) < 0)
                return AVERROR(ENOMEM);
            a->frame->nb_samples = a->frame_size;
            for (int ch = 0; ch < a->channels; ch++) {
                if (a->frame->format == AV_SAMPLE_FMT_FLTP) {
                    float *dst = (float *)a->frame->extended_data[ch];
                    for (int n = 0; n < a->frame_size; n++)
                        dst[n] = a->buf[n * a->channels + ch] / 32768.0f;
                } else {
                    int16_t *dst = (int16_t *)a->frame->extended_data[ch];
                    for (int n = 0; n < a->frame_size; n++)
                        dst[n] = a->buf[n * a->channels + ch];
                }
            }
            mobi_aenc_consume(a, a->frame_size);
            ret = avcodec_send_frame(a->enc, a->frame);
            if (ret < 0)
                return ret;
            continue;
        }

        if (a->draining) {
            if (a->nsamp > 0) {
                /* Pad the final partial frame with silence. */
                if (av_frame_make_writable(a->frame) < 0)
                    return AVERROR(ENOMEM);
                a->frame->nb_samples = a->frame_size;
                for (int ch = 0; ch < a->channels; ch++) {
                    if (a->frame->format == AV_SAMPLE_FMT_FLTP) {
                        float *dst = (float *)a->frame->extended_data[ch];
                        for (int n = 0; n < a->frame_size; n++)
                            dst[n] = (n < a->nsamp) ? a->buf[n * a->channels + ch] / 32768.0f : 0.0f;
                    } else {
                        int16_t *dst = (int16_t *)a->frame->extended_data[ch];
                        for (int n = 0; n < a->frame_size; n++)
                            dst[n] = (n < a->nsamp) ? a->buf[n * a->channels + ch] : 0;
                    }
                }
                a->nsamp = 0;
                ret = avcodec_send_frame(a->enc, a->frame);
                if (ret < 0)
                    return ret;
                continue;
            }
            /* No more input: signal EOF to the encoder once, then drain. */
            avcodec_send_frame(a->enc, NULL);
            ret = avcodec_receive_packet(a->enc, out);
            return ret; /* 0, AVERROR_EOF, or error */
        }
        return AVERROR(EAGAIN);
    }
}

static void mobi_aenc_close(MobiAudioEnc *a)
{
    av_frame_free(&a->frame);
    avcodec_free_context(&a->enc);
    av_freep(&a->buf);
    a->nsamp = a->cap = 0;
}

#endif /* AVFORMAT_MO_AUDIOENC_H */
