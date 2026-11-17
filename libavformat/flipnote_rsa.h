/*
 * Flipnote RSA signatures
 * Copyright (c) 2026 quatric - quatricsoftware@gmail.com
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

#ifndef AVFORMAT_FLIPNOTE_RSA_H
#define AVFORMAT_FLIPNOTE_RSA_H

#include <stdint.h>

/** Largest key the signer takes, in bytes (RSA-2048). */
#define FF_FLIPNOTE_RSA_MAX_BYTES 256

/**
 * RSASSA-PKCS1-v1_5 signature of data, with a SHA-1 (hash_bits 160) or
 * SHA-256 (hash_bits 256) digest. n and d are the big-endian modulus and
 * private exponent, key_bytes long (a multiple of 4, at most
 * FF_FLIPNOTE_RSA_MAX_BYTES); n must be odd and a full key_bytes long (top
 * bit set). sig receives key_bytes bytes. Self-contained, no external crypto library.
 *
 * @return 0 on success, a negative AVERROR code otherwise
 */
int ff_flipnote_rsa_sign(const uint8_t *n, const uint8_t *d, int key_bytes,
                         int hash_bits, const uint8_t *data, int64_t size,
                         uint8_t *sig);

#endif /* AVFORMAT_FLIPNOTE_RSA_H */
