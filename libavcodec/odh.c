/*
 * ODH (AJPG) image codec
 *
 * ODH is ActImagine/Nintendo's baseline-JPEG-derived still image format,
 * originally written for the Game Boy Advance and later reused for the Wii
 * Message Board's photo attachments.  Files start with the magic "AJPG"
 * followed by a 16-byte header; the entropy-coded data is plain 8x8 DCT
 * blocks with fixed Huffman trees and a JPEG-style quality-scaled quantizer.
 *
 * The compression core here is a port of the reference implementation used by
 * larsenv's cdbackup, which recovers Wii Message Board attachments.  That code
 * ran on a big endian console and read the header through native u32 loads;
 * this port goes through AV_RB32/AV_WB32 so the on-disk layout stays big
 * endian on every host.  The entropy coder itself is byte-oriented MSB-first
 * and needed no change.  The reference also emitted GameCube-tiled RGB565 /
 * RGBA8 / Y8U8V8 surfaces; that tiling is console glue rather than part of the
 * format, so it is dropped in favour of handing ffmpeg the planar YCbCr the
 * codec natively decodes to.
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

#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "encode.h"
#include "internal.h"

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

#define DArJCOL_RES_SUCCESS 0
#define DArJCOL_RES_ERROR   1

#define DArCONST_BITS 8
#define DArRANGE_MASK 0x3FF

#define DAr_FIX_0_382683433 ((s32)98)  /* FIX(0.382683433) */
#define DAr_FIX_0_541196100 ((s32)139) /* FIX(0.541196100) */
#define DAr_FIX_0_707106781 ((s32)181) /* FIX(0.707106781) */
#define DAr_FIX_1_306562965 ((s32)334) /* FIX(1.306562965) */
#define DArFIX_1_082392200  ((s32)277) /* FIX(1.082392200) */
#define DArFIX_1_414213562  ((s32)362) /* FIX(1.414213562) */
#define DArFIX_1_847759065  ((s32)473) /* FIX(1.847759065) */
#define DArFIX_2_613125930  ((s32)669) /* FIX(2.613125930) */

#define DArSCALEBITS 16

/* Number of quantization tables (luma, chroma). */
#define DArCDJ_QUANTIZE_TABLE_NUM 2
/* DCT block edge, and its square. */
#define DArCDJ_DCT_SIZE_1D 8
#define DArCDJ_DCT_SIZE_2D (DArCDJ_DCT_SIZE_1D * DArCDJ_DCT_SIZE_1D)

/* Indices into the two-element X/Y arrays. */
#define DArCDJ_AXIS_X 0
#define DArCDJ_AXIS_Y 1
#define DArCDJ_IMAGE_DIMENSION 2

/* Indices into the three-element YCbCr arrays. */
#define DArCDJ_COLOR_Y  0
#define DArCDJ_COLOR_Cb 1
#define DArCDJ_COLOR_Cr 2
#define DArCDJ_COLOR_DIMENSION 3

/* DCT scratch, in bytes (4-byte aligned). */
#define DArCDJ_DCT_BUFFER_SIZE 512

/* Coefficients in one 8x8 block; every DCTBlock write must stay under it. */
#define ODH_BLOCK_COEFFS (DArCDJ_DCT_BUFFER_SIZE >> 3)

/* Largest dimensions the 11-bit header fields can express. */
#define DArCDJ_PIXEL_SIZE_MAX_X 0x07FF
#define DArCDJ_PIXEL_SIZE_MAX_Y 0x07FF

#define DArCDJ_HEADER_SIZE 16

/*
 * Both directions carry a 1-or-2 chroma sampling ratio, giving the four
 * combinations the header's two rate bits can express.  The reference fixed
 * these at build time and only ever shipped 4:4:4; here they follow the
 * input pixel format.
 */

#define DArCDJRESULT_ERR_BIT 0x80000000

#define DArCDJRESULT_SUCCESS          0x00000000
#define DArCDJRESULT_ERR_SIZE         (0x00000001 | DArCDJRESULT_ERR_BIT)
#define DArCDJRESULT_ERR_QUALITY      (0x00000002 | DArCDJRESULT_ERR_BIT)
#define DArCDJRESULT_ERR_HUFFCODE     (0x00000003 | DArCDJRESULT_ERR_BIT)
#define DArCDJRESULT_ERR_CODE_SIZE    (0x00000004 | DArCDJRESULT_ERR_BIT)
#define DArCDJRESULT_ERR_INVALID_DATA (0x00000005 | DArCDJRESULT_ERR_BIT)

#define DArDESCALE(x, n) ((x) >> (n))
#define DArMULTIPLY(var, const) (DArDESCALE((var) * (const), DArCONST_BITS))

typedef struct {
    u32 *mPreviousDC[2];       /* previous DC coefficients */
    const u32 *mDCTable;
    const u32 *mACTable;
    u8 *mCodeBuffer;
    /* One past the last readable coded byte; decode side only. */
    const u8 *mCodeBufferEnd;
    u32 *mCodeBufferRemain;
    u32 *mHuffmanBuffer;
    u32 *mHuffmanBufferRemain;
    u32 mCodeBufferSize;
} SArCDJ_HuffmanRequest;

typedef struct {
    u16 mImageSize[DArCDJ_IMAGE_DIMENSION];
    u8  mQuality;
    u16 mMCUinImage[DArCDJ_IMAGE_DIMENSION];
    u16 mCurrentMCU[DArCDJ_IMAGE_DIMENSION];

    /* Cb/Cr downsampling ratio; read from the header when decoding. */
    u8 mSmpRate[DArCDJ_IMAGE_DIMENSION];

    u32 mHuffmanBuffer;
    u32 mHuffmanBits;
    u32 mCodeBufferSize;
    u32 mRemainCodeBuffer;

    u32 mPreviousDC[DArCDJ_COLOR_DIMENSION];

    u8 *mImgYCbCrBufferPtr;
    u32 mDCTBuffer[DArCDJ_DCT_BUFFER_SIZE / sizeof(u32)];
    u8 *mCodeBufferPtr;
    u32 mQuantizationTable[DArCDJ_QUANTIZE_TABLE_NUM][DArCDJ_DCT_SIZE_2D];

    SArCDJ_HuffmanRequest mHuffmanRequest[2];
} SArCDJ_OdhMaster;
static const u8 odh_zigzag_order[DArCDJ_DCT_SIZE_2D] = {
    0, 1, 5, 6, 14, 15, 27, 28,
    2, 4, 7, 13, 16, 26, 29, 42,
    3, 8, 12, 17, 25, 30, 41, 43,
    9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63};

static const u8 FDCTOrder1[8] = {
    0, 32, 16, 48, 40, 8, 56, 24};

static const u8 FDCTOrder2[8] = {
    5, 1, 7, 3, 2, 6, 0, 4};

static const u8 odh_natural_order[DArCDJ_DCT_SIZE_2D] = {
    0, 1, 8, 16, 9, 2, 3, 10,
    17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63};

static const u8 range_limit[1024] = {
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F,
    0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
    0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF,
    0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF,
    0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF,
    0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF,
    0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F,
    0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F,
    0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F};

static const u8 gArCdj_std_quant_tbl[2][64] = {
    {16, 11, 10, 16, 24, 40, 51, 61,
     12, 12, 14, 19, 26, 58, 60, 55,
     14, 13, 16, 24, 40, 57, 69, 56,
     14, 17, 22, 29, 51, 87, 80, 62,
     18, 22, 37, 56, 68, 109, 103, 77,
     24, 35, 55, 64, 81, 104, 113, 92,
     49, 64, 78, 87, 103, 121, 120, 101,
     72, 92, 95, 98, 112, 100, 103, 99},
    {17, 18, 24, 47, 99, 99, 99, 99,
     18, 21, 26, 66, 99, 99, 99, 99,
     24, 26, 56, 99, 99, 99, 99, 99,
     47, 66, 99, 99, 99, 99, 99, 99,
     99, 99, 99, 99, 99, 99, 99, 99,
     99, 99, 99, 99, 99, 99, 99, 99,
     99, 99, 99, 99, 99, 99, 99, 99,
     99, 99, 99, 99, 99, 99, 99, 99}};

static const u16 gArAANScales[64] = {
    16384, 22725, 21407, 19266, 16384, 12873, 8867, 4520,
    22725, 31521, 29692, 26722, 22725, 17855, 12299, 6270,
    21407, 29692, 27969, 25172, 21407, 16819, 11585, 5906,
    19266, 26722, 25172, 22654, 19266, 15137, 10426, 5315,
    16384, 22725, 21407, 19266, 16384, 12873, 8867, 4520,
    12873, 17855, 16819, 15137, 12873, 10114, 6967, 3552,
    8867, 12299, 11585, 10426, 8867, 6967, 4799, 2446,
    4520, 6270, 5906, 5315, 4520, 3552, 2446, 1247};

static const u32 gArDC_L_Table[16] = {
    0x02000000, 0x03000002, 0x03000003, 0x03000004, 0x03000005, 0x03000006, 0x0400000e, 0x0500001e,
    0x0600003e, 0x0700007e, 0x080000fe, 0x090001fe, 0x00000000, 0x00000000, 0x00000000, 0x00000000};

static const u32 gArAC_L_Table[16 * 16] = {
    0x0400000a, 0x02000000, 0x02000001, 0x03000004, 0x0400000b, 0x0500001a, 0x07000078, 0x080000f8,
    0x0a0003f6, 0x1000ff82, 0x1000ff83, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x0400000c, 0x0500001b, 0x07000079, 0x090001f6, 0x0b0007f6, 0x1000ff84, 0x1000ff85,
    0x1000ff86, 0x1000ff87, 0x1000ff88, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x0500001c, 0x080000f9, 0x0a0003f7, 0x0c000ff4, 0x1000ff89, 0x1000ff8a, 0x1000ff8b,
    0x1000ff8c, 0x1000ff8d, 0x1000ff8e, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x0600003a, 0x090001f7, 0x0c000ff5, 0x1000ff8f, 0x1000ff90, 0x1000ff91, 0x1000ff92,
    0x1000ff93, 0x1000ff94, 0x1000ff95, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x0600003b, 0x0a0003f8, 0x1000ff96, 0x1000ff97, 0x1000ff98, 0x1000ff99, 0x1000ff9a,
    0x1000ff9b, 0x1000ff9c, 0x1000ff9d, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x0700007a, 0x0b0007f7, 0x1000ff9e, 0x1000ff9f, 0x1000ffa0, 0x1000ffa1, 0x1000ffa2,
    0x1000ffa3, 0x1000ffa4, 0x1000ffa5, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x0700007b, 0x0c000ff6, 0x1000ffa6, 0x1000ffa7, 0x1000ffa8, 0x1000ffa9, 0x1000ffaa,
    0x1000ffab, 0x1000ffac, 0x1000ffad, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x080000fa, 0x0c000ff7, 0x1000ffae, 0x1000ffaf, 0x1000ffb0, 0x1000ffb1, 0x1000ffb2,
    0x1000ffb3, 0x1000ffb4, 0x1000ffb5, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x090001f8, 0x0f007fc0, 0x1000ffb6, 0x1000ffb7, 0x1000ffb8, 0x1000ffb9, 0x1000ffba,
    0x1000ffbb, 0x1000ffbc, 0x1000ffbd, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x090001f9, 0x1000ffbe, 0x1000ffbf, 0x1000ffc0, 0x1000ffc1, 0x1000ffc2, 0x1000ffc3,
    0x1000ffc4, 0x1000ffc5, 0x1000ffc6, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x090001fa, 0x1000ffc7, 0x1000ffc8, 0x1000ffc9, 0x1000ffca, 0x1000ffcb, 0x1000ffcc,
    0x1000ffcd, 0x1000ffce, 0x1000ffcf, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x0a0003f9, 0x1000ffd0, 0x1000ffd1, 0x1000ffd2, 0x1000ffd3, 0x1000ffd4, 0x1000ffd5,
    0x1000ffd6, 0x1000ffd7, 0x1000ffd8, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x0a0003fa, 0x1000ffd9, 0x1000ffda, 0x1000ffdb, 0x1000ffdc, 0x1000ffdd, 0x1000ffde,
    0x1000ffdf, 0x1000ffe0, 0x1000ffe1, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x0b0007f8, 0x1000ffe2, 0x1000ffe3, 0x1000ffe4, 0x1000ffe5, 0x1000ffe6, 0x1000ffe7,
    0x1000ffe8, 0x1000ffe9, 0x1000ffea, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x1000ffeb, 0x1000ffec, 0x1000ffed, 0x1000ffee, 0x1000ffef, 0x1000fff0, 0x1000fff1,
    0x1000fff2, 0x1000fff3, 0x1000fff4, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x0b0007f9, 0x1000fff5, 0x1000fff6, 0x1000fff7, 0x1000fff8, 0x1000fff9, 0x1000fffa, 0x1000fffb,
    0x1000fffc, 0x1000fffd, 0x1000fffe, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000};

static const u32 gArDC_C_Table[16] = {
    0x02000000, 0x02000001, 0x02000002, 0x03000006, 0x0400000e, 0x0500001e, 0x0600003e, 0x0700007e,
    0x080000fe, 0x090001fe, 0x0a0003fe, 0x0b0007fe, 0x00000000, 0x00000000, 0x00000000, 0x00000000};

static const u32 gArAC_C_Table[16 * 16] = {
    0x02000000, 0x02000001, 0x03000004, 0x0400000a, 0x05000018, 0x05000019, 0x06000038, 0x07000078,
    0x090001f4, 0x0a0003f6, 0x0c000ff4, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x0400000b, 0x06000039, 0x080000f6, 0x090001f5, 0x0b0007f6, 0x0c000ff5, 0x1000ff88,
    0x1000ff89, 0x1000ff8a, 0x1000ff8b, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x0500001a, 0x080000f7, 0x0a0003f7, 0x0c000ff6, 0x0f007fc2, 0x1000ff8c, 0x1000ff8d,
    0x1000ff8e, 0x1000ff8f, 0x1000ff90, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x0500001b, 0x080000f8, 0x0a0003f8, 0x0c000ff7, 0x1000ff91, 0x1000ff92, 0x1000ff93,
    0x1000ff94, 0x1000ff95, 0x1000ff96, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x0600003a, 0x090001f6, 0x1000ff97, 0x1000ff98, 0x1000ff99, 0x1000ff9a, 0x1000ff9b,
    0x1000ff9c, 0x1000ff9d, 0x1000ff9e, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x0600003b, 0x0a0003f9, 0x1000ff9f, 0x1000ffa0, 0x1000ffa1, 0x1000ffa2, 0x1000ffa3,
    0x1000ffa4, 0x1000ffa5, 0x1000ffa6, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x07000079, 0x0b0007f7, 0x1000ffa7, 0x1000ffa8, 0x1000ffa9, 0x1000ffaa, 0x1000ffab,
    0x1000ffac, 0x1000ffad, 0x1000ffae, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x0700007a, 0x0b0007f8, 0x1000ffaf, 0x1000ffb0, 0x1000ffb1, 0x1000ffb2, 0x1000ffb3,
    0x1000ffb4, 0x1000ffb5, 0x1000ffb6, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x080000f9, 0x1000ffb7, 0x1000ffb8, 0x1000ffb9, 0x1000ffba, 0x1000ffbb, 0x1000ffbc,
    0x1000ffbd, 0x1000ffbe, 0x1000ffbf, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x090001f7, 0x1000ffc0, 0x1000ffc1, 0x1000ffc2, 0x1000ffc3, 0x1000ffc4, 0x1000ffc5,
    0x1000ffc6, 0x1000ffc7, 0x1000ffc8, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x090001f8, 0x1000ffc9, 0x1000ffca, 0x1000ffcb, 0x1000ffcc, 0x1000ffcd, 0x1000ffce,
    0x1000ffcf, 0x1000ffd0, 0x1000ffd1, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x090001f9, 0x1000ffd2, 0x1000ffd3, 0x1000ffd4, 0x1000ffd5, 0x1000ffd6, 0x1000ffd7,
    0x1000ffd8, 0x1000ffd9, 0x1000ffda, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x090001fa, 0x1000ffdb, 0x1000ffdc, 0x1000ffdd, 0x1000ffde, 0x1000ffdf, 0x1000ffe0,
    0x1000ffe1, 0x1000ffe2, 0x1000ffe3, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x0b0007f9, 0x1000ffe4, 0x1000ffe5, 0x1000ffe6, 0x1000ffe7, 0x1000ffe8, 0x1000ffe9,
    0x1000ffea, 0x1000ffeb, 0x1000ffec, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x0e003fe0, 0x1000ffed, 0x1000ffee, 0x1000ffef, 0x1000fff0, 0x1000fff1, 0x1000fff2,
    0x1000fff3, 0x1000fff4, 0x1000fff5, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x0a0003fa, 0x0f007fc3, 0x1000fff6, 0x1000fff7, 0x1000fff8, 0x1000fff9, 0x1000fffa, 0x1000fffb,
    0x1000fffc, 0x1000fffd, 0x1000fffe, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000};

// cdj_c_plttTable.h Const Data
static const u16 gArDc_luminance_huffTable[24] = {
    0x0001,
    0x8002,
    0x0003,
    0x0000,
    0xc003,
    0xc004,
    0x8005,
    0x0001,
    0x0002,
    0x0003,
    0x0004,
    0x0005,
    0x8001,
    0x0006,
    0x8001,
    0x0007,
    0x8001,
    0x0008,
    0x8001,
    0x0009,
    0x8001,
    0x000a,
    0x8001,
    0x000b,
};

static const u16 gArDc_chrominance_huffTable[24] = {
    0x0001,
    0xc002,
    0x8003,
    0x0000,
    0x0001,
    0x0002,
    0x8001,
    0x0003,
    0x8001,
    0x0004,
    0x8001,
    0x0005,
    0x8001,
    0x0006,
    0x8001,
    0x0007,
    0x8001,
    0x0008,
    0x8001,
    0x0009,
    0x8001,
    0x000a,
    0x8001,
    0x000b,
};

static const u16 gArAc_luminance_huffTable[324] = {
    0x0001,
    0xc002,
    0x0003,
    0x0001,
    0x0002,
    0x8002,
    0x0003,
    0x0003,
    0xc003,
    0x8004,
    0x0005,
    0x0000,
    0x0004,
    0x0011,
    0xc003,
    0x8004,
    0x0005,
    0x0005,
    0x0012,
    0x0021,
    0xc003,
    0x0004,
    0x0005,
    0x0031,
    0x0041,
    0xc004,
    0xc005,
    0x0006,
    0x0007,
    0x0006,
    0x0013,
    0x0051,
    0x0061,
    0xc004,
    0x8005,
    0x0006,
    0x0007,
    0x0007,
    0x0022,
    0x0071,
    0xc005,
    0xc006,
    0x8007,
    0x0008,
    0x0009,
    0x0014,
    0x0032,
    0x0081,
    0x0091,
    0x00a1,
    0xc005,
    0xc006,
    0x8007,
    0x0008,
    0x0009,
    0x0008,
    0x0023,
    0x0042,
    0x00b1,
    0x00c1,
    0xc005,
    0xc006,
    0x0007,
    0x0008,
    0x0009,
    0x0015,
    0x0052,
    0x00d1,
    0x00f0,
    0xc006,
    0xc007,
    0x0008,
    0x0009,
    0x000a,
    0x000b,
    0x0024,
    0x0033,
    0x0062,
    0x0072,
    0x0008,
    0x0009,
    0x000a,
    0x000b,
    0x000c,
    0x000d,
    0x000e,
    0x000f,
    0x0010,
    0x0011,
    0x0012,
    0x0013,
    0x0014,
    0x0015,
    0x0016,
    0x0017,
    0x0018,
    0x0019,
    0x001a,
    0x001b,
    0x001c,
    0x001d,
    0x001e,
    0x001f,
    0x8020,
    0x0021,
    0x0022,
    0x0023,
    0x0024,
    0x0025,
    0x0026,
    0x0027,
    0x0028,
    0x0029,
    0x002a,
    0x002b,
    0x002c,
    0x002d,
    0x002e,
    0x002f,
    0x0030,
    0x0031,
    0x0032,
    0x0033,
    0x0034,
    0x0035,
    0x0036,
    0x0037,
    0x0038,
    0x0039,
    0x003a,
    0x003b,
    0x003c,
    0x003d,
    0x003e,
    0x003f,
    0x0082,
    0xc03f,
    0xc040,
    0xc041,
    0xc042,
    0xc043,
    0xc044,
    0xc045,
    0xc046,
    0xc047,
    0xc048,
    0xc049,
    0xc04a,
    0xc04b,
    0xc04c,
    0xc04d,
    0xc04e,
    0xc04f,
    0xc050,
    0xc051,
    0xc052,
    0xc053,
    0xc054,
    0xc055,
    0xc056,
    0xc057,
    0xc058,
    0xc059,
    0xc05a,
    0xc05b,
    0xc05c,
    0xc05d,
    0xc05e,
    0xc05f,
    0xc060,
    0xc061,
    0xc062,
    0xc063,
    0xc064,
    0xc065,
    0xc066,
    0xc067,
    0xc068,
    0xc069,
    0xc06a,
    0xc06b,
    0xc06c,
    0xc06d,
    0xc06e,
    0xc06f,
    0xc070,
    0xc071,
    0xc072,
    0xc073,
    0xc074,
    0xc075,
    0xc076,
    0xc077,
    0xc078,
    0xc079,
    0xc07a,
    0xc07b,
    0xc07c,
    0x807d,
    0x0009,
    0x000a,
    0x0016,
    0x0017,
    0x0018,
    0x0019,
    0x001a,
    0x0025,
    0x0026,
    0x0027,
    0x0028,
    0x0029,
    0x002a,
    0x0034,
    0x0035,
    0x0036,
    0x0037,
    0x0038,
    0x0039,
    0x003a,
    0x0043,
    0x0044,
    0x0045,
    0x0046,
    0x0047,
    0x0048,
    0x0049,
    0x004a,
    0x0053,
    0x0054,
    0x0055,
    0x0056,
    0x0057,
    0x0058,
    0x0059,
    0x005a,
    0x0063,
    0x0064,
    0x0065,
    0x0066,
    0x0067,
    0x0068,
    0x0069,
    0x006a,
    0x0073,
    0x0074,
    0x0075,
    0x0076,
    0x0077,
    0x0078,
    0x0079,
    0x007a,
    0x0083,
    0x0084,
    0x0085,
    0x0086,
    0x0087,
    0x0088,
    0x0089,
    0x008a,
    0x0092,
    0x0093,
    0x0094,
    0x0095,
    0x0096,
    0x0097,
    0x0098,
    0x0099,
    0x009a,
    0x00a2,
    0x00a3,
    0x00a4,
    0x00a5,
    0x00a6,
    0x00a7,
    0x00a8,
    0x00a9,
    0x00aa,
    0x00b2,
    0x00b3,
    0x00b4,
    0x00b5,
    0x00b6,
    0x00b7,
    0x00b8,
    0x00b9,
    0x00ba,
    0x00c2,
    0x00c3,
    0x00c4,
    0x00c5,
    0x00c6,
    0x00c7,
    0x00c8,
    0x00c9,
    0x00ca,
    0x00d2,
    0x00d3,
    0x00d4,
    0x00d5,
    0x00d6,
    0x00d7,
    0x00d8,
    0x00d9,
    0x00da,
    0x00e1,
    0x00e2,
    0x00e3,
    0x00e4,
    0x00e5,
    0x00e6,
    0x00e7,
    0x00e8,
    0x00e9,
    0x00ea,
    0x00f1,
    0x00f2,
    0x00f3,
    0x00f4,
    0x00f5,
    0x00f6,
    0x00f7,
    0x00f8,
    0x00f9,
    0x00fa,
};

static const u16 gArAc_chrominance_huffTable[324] = {
    0x0001,
    0xc002,
    0x0003,
    0x0000,
    0x0001,
    0x8002,
    0x0003,
    0x0002,
    0xc003,
    0x0004,
    0x0005,
    0x0003,
    0x0011,
    0xc004,
    0xc005,
    0x0006,
    0x0007,
    0x0004,
    0x0005,
    0x0021,
    0x0031,
    0xc004,
    0xc005,
    0x0006,
    0x0007,
    0x0006,
    0x0012,
    0x0041,
    0x0051,
    0xc004,
    0x8005,
    0x0006,
    0x0007,
    0x0007,
    0x0061,
    0x0071,
    0xc005,
    0xc006,
    0x0007,
    0x0008,
    0x0009,
    0x0013,
    0x0022,
    0x0032,
    0x0081,
    0xc006,
    0xc007,
    0xc008,
    0x8009,
    0x000a,
    0x000b,
    0x0008,
    0x0014,
    0x0042,
    0x0091,
    0x00a1,
    0x00b1,
    0x00c1,
    0xc005,
    0xc006,
    0x8007,
    0x0008,
    0x0009,
    0x0009,
    0x0023,
    0x0033,
    0x0052,
    0x00f0,
    0xc005,
    0xc006,
    0x0007,
    0x0008,
    0x0009,
    0x0015,
    0x0062,
    0x0072,
    0x00d1,
    0xc006,
    0xc007,
    0x0008,
    0x0009,
    0x000a,
    0x000b,
    0x000a,
    0x0016,
    0x0024,
    0x0034,
    0x0008,
    0x0009,
    0x000a,
    0x000b,
    0x000c,
    0x000d,
    0x000e,
    0x000f,
    0x8010,
    0x0011,
    0x0012,
    0x0013,
    0x0014,
    0x0015,
    0x0016,
    0x0017,
    0x0018,
    0x0019,
    0x001a,
    0x001b,
    0x001c,
    0x001d,
    0x001e,
    0x001f,
    0x00e1,
    0xc01f,
    0x0020,
    0x0021,
    0x0022,
    0x0023,
    0x0024,
    0x0025,
    0x0026,
    0x0027,
    0x0028,
    0x0029,
    0x002a,
    0x002b,
    0x002c,
    0x002d,
    0x002e,
    0x002f,
    0x0030,
    0x0031,
    0x0032,
    0x0033,
    0x0034,
    0x0035,
    0x0036,
    0x0037,
    0x0038,
    0x0039,
    0x003a,
    0x003b,
    0x003c,
    0x003d,
    0x0025,
    0x00f1,
    0xc03c,
    0xc03d,
    0xc03e,
    0xc03f,
    0xc040,
    0xc041,
    0xc042,
    0xc043,
    0xc044,
    0xc045,
    0xc046,
    0xc047,
    0xc048,
    0xc049,
    0xc04a,
    0xc04b,
    0xc04c,
    0xc04d,
    0xc04e,
    0xc04f,
    0xc050,
    0xc051,
    0xc052,
    0xc053,
    0xc054,
    0xc055,
    0xc056,
    0xc057,
    0xc058,
    0xc059,
    0xc05a,
    0xc05b,
    0xc05c,
    0xc05d,
    0xc05e,
    0xc05f,
    0xc060,
    0xc061,
    0xc062,
    0xc063,
    0xc064,
    0xc065,
    0xc066,
    0xc067,
    0xc068,
    0xc069,
    0xc06a,
    0xc06b,
    0xc06c,
    0xc06d,
    0xc06e,
    0xc06f,
    0xc070,
    0xc071,
    0xc072,
    0xc073,
    0xc074,
    0xc075,
    0xc076,
    0x8077,
    0x0017,
    0x0018,
    0x0019,
    0x001a,
    0x0026,
    0x0027,
    0x0028,
    0x0029,
    0x002a,
    0x0035,
    0x0036,
    0x0037,
    0x0038,
    0x0039,
    0x003a,
    0x0043,
    0x0044,
    0x0045,
    0x0046,
    0x0047,
    0x0048,
    0x0049,
    0x004a,
    0x0053,
    0x0054,
    0x0055,
    0x0056,
    0x0057,
    0x0058,
    0x0059,
    0x005a,
    0x0063,
    0x0064,
    0x0065,
    0x0066,
    0x0067,
    0x0068,
    0x0069,
    0x006a,
    0x0073,
    0x0074,
    0x0075,
    0x0076,
    0x0077,
    0x0078,
    0x0079,
    0x007a,
    0x0082,
    0x0083,
    0x0084,
    0x0085,
    0x0086,
    0x0087,
    0x0088,
    0x0089,
    0x008a,
    0x0092,
    0x0093,
    0x0094,
    0x0095,
    0x0096,
    0x0097,
    0x0098,
    0x0099,
    0x009a,
    0x00a2,
    0x00a3,
    0x00a4,
    0x00a5,
    0x00a6,
    0x00a7,
    0x00a8,
    0x00a9,
    0x00aa,
    0x00b2,
    0x00b3,
    0x00b4,
    0x00b5,
    0x00b6,
    0x00b7,
    0x00b8,
    0x00b9,
    0x00ba,
    0x00c2,
    0x00c3,
    0x00c4,
    0x00c5,
    0x00c6,
    0x00c7,
    0x00c8,
    0x00c9,
    0x00ca,
    0x00d2,
    0x00d3,
    0x00d4,
    0x00d5,
    0x00d6,
    0x00d7,
    0x00d8,
    0x00d9,
    0x00da,
    0x00e2,
    0x00e3,
    0x00e4,
    0x00e5,
    0x00e6,
    0x00e7,
    0x00e8,
    0x00e9,
    0x00ea,
    0x00f2,
    0x00f3,
    0x00f4,
    0x00f5,
    0x00f6,
    0x00f7,
    0x00f8,
    0x00f9,
    0x00fa,
};

static const u16 *const hufftreePtr[4] = {
    gArDc_luminance_huffTable, gArDc_luminance_huffTable,
    gArDc_chrominance_huffTable, gArDc_luminance_huffTable};

/*------------------------------------------------------------------*/

/* The reference implementation is laid out caller-first, so the helpers each
 * stage reaches for are defined further down.  Declare them up front rather
 * than reordering the port away from the source it is checked against. */
static u32  cdj_c_flashBuffer(SArCDJ_OdhMaster *cdj_ctrl);
static void cdj_c_setQuantizationTable(SArCDJ_OdhMaster *cdj_ctrl, u32 quality);
static void cdj_c_makeHeader(SArCDJ_OdhMaster *cdj_ctrl, u32 fileSize);
static void fdct_fast(u32 *dctBlock, u8 *pixelBuffer, u32 pixelOffset,
                      u32 *quantTable);
static u32  huffmanCoder(u16 *DCTBuffer, SArCDJ_HuffmanRequest *huffRequest);
static void cdj_d_setDequantizationTable(SArCDJ_OdhMaster *cdj_ctrl, u32 quality);
static u32  huffmanDecoder(u32 *DCTBlock, SArCDJ_HuffmanRequest *huffmanRequest,
                           const u16 *const *hufftree, int col);
static void idct_fast(const u8 *range_limit, u32 *quant_table, u32 *DCTBuffer,
                      u8 *output_buf, u32 pixelOffset);

static u32 cdj_c_initializeCompressOdh(
    SArCDJ_OdhMaster *cdj_ctrl, u16 *imageSize, u8 quality,
    u8 *imgYCbCrBufPtr, u8 *CodeBufPtr, u32 CodeBufSize)
{
    u32 qualityEx;

    
    
    if ((!imageSize[DArCDJ_AXIS_X]) || (imageSize[DArCDJ_AXIS_X] > DArCDJ_PIXEL_SIZE_MAX_X) ||
        (!imageSize[DArCDJ_AXIS_Y]) || (imageSize[DArCDJ_AXIS_Y] > DArCDJ_PIXEL_SIZE_MAX_Y))
    {
        return DArCDJRESULT_ERR_SIZE;
    }

    
    if (quality > 100)
    {
        return DArCDJRESULT_ERR_QUALITY;
    }

    
    cdj_ctrl->mImgYCbCrBufferPtr = imgYCbCrBufPtr;
    
    cdj_ctrl->mCodeBufferPtr = CodeBufPtr;

    
    cdj_ctrl->mImageSize[DArCDJ_AXIS_X] = imageSize[DArCDJ_AXIS_X];
    cdj_ctrl->mImageSize[DArCDJ_AXIS_Y] = imageSize[DArCDJ_AXIS_Y];
    cdj_ctrl->mQuality = quality;

    
    cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] =
        (u16)((imageSize[DArCDJ_AXIS_X] - 1) /
              (DArCDJ_DCT_SIZE_1D * cdj_ctrl->mSmpRate[DArCDJ_AXIS_X]) + 1);
    cdj_ctrl->mMCUinImage[DArCDJ_AXIS_Y] =
        (u16)((imageSize[DArCDJ_AXIS_Y] - 1) /
              (DArCDJ_DCT_SIZE_1D * cdj_ctrl->mSmpRate[DArCDJ_AXIS_Y]) + 1);

    
    cdj_ctrl->mHuffmanRequest[0].mPreviousDC[0] = cdj_ctrl->mPreviousDC;
    cdj_ctrl->mHuffmanRequest[0].mPreviousDC[1] = cdj_ctrl->mPreviousDC;
    cdj_ctrl->mHuffmanRequest[0].mDCTable = gArDC_L_Table;
    cdj_ctrl->mHuffmanRequest[0].mACTable = gArDC_L_Table; // gArAC_L_Table;
    cdj_ctrl->mHuffmanRequest[0].mCodeBuffer = cdj_ctrl->mCodeBufferPtr;
    cdj_ctrl->mHuffmanRequest[0].mCodeBufferRemain = &cdj_ctrl->mRemainCodeBuffer;
    cdj_ctrl->mHuffmanRequest[0].mHuffmanBuffer = &cdj_ctrl->mHuffmanBuffer;
    cdj_ctrl->mHuffmanRequest[0].mHuffmanBufferRemain = &cdj_ctrl->mHuffmanBits;
    cdj_ctrl->mHuffmanRequest[0].mCodeBufferSize = CodeBufSize;
    cdj_ctrl->mHuffmanRequest[1].mPreviousDC[0] = cdj_ctrl->mPreviousDC + 1;
    cdj_ctrl->mHuffmanRequest[1].mPreviousDC[1] = cdj_ctrl->mPreviousDC + 2;
    cdj_ctrl->mHuffmanRequest[1].mDCTable = gArDC_C_Table;
    cdj_ctrl->mHuffmanRequest[1].mACTable = gArDC_L_Table; // gArAC_C_Table;
    cdj_ctrl->mHuffmanRequest[1].mCodeBuffer = cdj_ctrl->mCodeBufferPtr;
    cdj_ctrl->mHuffmanRequest[1].mCodeBufferRemain = &cdj_ctrl->mRemainCodeBuffer;
    cdj_ctrl->mHuffmanRequest[1].mHuffmanBuffer = &cdj_ctrl->mHuffmanBuffer;
    cdj_ctrl->mHuffmanRequest[1].mHuffmanBufferRemain = &cdj_ctrl->mHuffmanBits;
    cdj_ctrl->mHuffmanRequest[1].mCodeBufferSize = CodeBufSize;

    
    qualityEx = (u32)quality;
    if (qualityEx <= 0)
        qualityEx = 1;
    if (qualityEx < 50)
        qualityEx = (u32)(5000 / qualityEx);
    else
        qualityEx = (u32)(200 - qualityEx * 2);

    cdj_c_setQuantizationTable(cdj_ctrl, qualityEx);

    
    
    cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X] = 0;
    cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y] = 0;

    
    cdj_ctrl->mHuffmanBuffer = 0;
    
    cdj_ctrl->mHuffmanBits = 32;
    
    cdj_ctrl->mCodeBufferSize = CodeBufSize;
    
    
    cdj_ctrl->mRemainCodeBuffer = CodeBufSize - DArCDJ_HEADER_SIZE;

    
    cdj_ctrl->mPreviousDC[DArCDJ_COLOR_Y] = 0;
    cdj_ctrl->mPreviousDC[DArCDJ_COLOR_Cb] = 0;
    cdj_ctrl->mPreviousDC[DArCDJ_COLOR_Cr] = 0;

    return DArCDJRESULT_SUCCESS;
}

/********************************************************************/

/********************************************************************/
/*
 * The entropy coder interleaves two 8x8 blocks through one u32 scratch array,
 * which it then walks as u16 with a stride of 2.  The reference packed the
 * first block into the top half of each word and the second into the bottom,
 * which only lines up with that walk on a big endian host.  Address the two
 * u16 slots by position instead, so the packing means the same thing
 * everywhere.
 */
#define ODH_DCT_SLOT0(ctrl, n) (((u16 *)(ctrl)->mDCTBuffer)[2 * (n)])
#define ODH_DCT_SLOT1(ctrl, n) (((u16 *)(ctrl)->mDCTBuffer)[2 * (n) + 1])

static u32 cdj_c_compressLoop(SArCDJ_OdhMaster *cdj_ctrl)
{
    u8 *pixBlockPtr;
    int idx, i;
    u32 result;
    const int sampX = cdj_ctrl->mSmpRate[DArCDJ_AXIS_X];
    const int sampY = cdj_ctrl->mSmpRate[DArCDJ_AXIS_Y];

    
    do
    {

    if (sampX == 1 && sampY == 1)
    {
        
        
        idx = cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X] * DArCDJ_DCT_SIZE_1D + cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y] * DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X];
        pixBlockPtr = &(cdj_ctrl->mImgYCbCrBufferPtr[idx]);

        // DCT
        fdct_fast(&cdj_ctrl->mDCTBuffer[64], pixBlockPtr,
                  (u32)(DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X]),
                  cdj_ctrl->mQuantizationTable[0]);

        
        for (i = 0; i < DArCDJ_DCT_SIZE_2D; i++)
        {
            ODH_DCT_SLOT0(cdj_ctrl, odh_zigzag_order[i]) =
                (u16)cdj_ctrl->mDCTBuffer[64 + i];
        }
        cdj_ctrl->mDCTBuffer[64] = 0x40004000;

        
        // HuffmanCoderWram(((u16 *)cdj_ctrl->mDCTBuffer)-1,
        //                 cdj_ctrl->mHuffmanRequest);
        result = huffmanCoder(((u16 *)cdj_ctrl->mDCTBuffer) - 1, cdj_ctrl->mHuffmanRequest);

        
        if (result == DArCDJRESULT_ERR_CODE_SIZE)
        {
            return DArCDJRESULT_ERR_CODE_SIZE;
        }

        
        
        idx += DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_Y];
        pixBlockPtr = &(cdj_ctrl->mImgYCbCrBufferPtr[idx]);

        // DCT
        fdct_fast(&cdj_ctrl->mDCTBuffer[64], pixBlockPtr,
                  (u32)(DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X]),
                  cdj_ctrl->mQuantizationTable[1]);

        
        for (i = 0; i < DArCDJ_DCT_SIZE_2D; i++)
        {
            ODH_DCT_SLOT1(cdj_ctrl, odh_zigzag_order[i]) =
                (u16)cdj_ctrl->mDCTBuffer[64 + i];
        }

        
        
        idx += DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_Y];
        pixBlockPtr = &(cdj_ctrl->mImgYCbCrBufferPtr[idx]);

        // DCT
        fdct_fast(&cdj_ctrl->mDCTBuffer[64], pixBlockPtr,
                  (u32)(DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X]),
                  cdj_ctrl->mQuantizationTable[1]);

        
        for (i = 0; i < DArCDJ_DCT_SIZE_2D; i++)
        {
            
            ODH_DCT_SLOT0(cdj_ctrl, odh_zigzag_order[i]) =
                (u16)cdj_ctrl->mDCTBuffer[64 + i];
        }
        cdj_ctrl->mDCTBuffer[64] = 0x40004000;

        
        // HuffmanCoderWram((u16 *)cdj_ctrl->mDCTBuffer,
        //                 cdj_ctrl->mHuffmanRequest+1);
        result = huffmanCoder((u16 *)cdj_ctrl->mDCTBuffer, cdj_ctrl->mHuffmanRequest + 1);

        
        if (result == DArCDJRESULT_ERR_CODE_SIZE)
        {
            return DArCDJRESULT_ERR_CODE_SIZE;
        }

    }
    else if (sampX == 2 && sampY == 1)
    {
        
        
        idx = cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X] * DArCDJ_DCT_SIZE_1D * 2 + cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y] * DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * 2;
        pixBlockPtr = &(cdj_ctrl->mImgYCbCrBufferPtr[idx]);

        // DCT
        fdct_fast(&cdj_ctrl->mDCTBuffer[64], pixBlockPtr,
                  (u32)(DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * 2),
                  cdj_ctrl->mQuantizationTable[0]);

        
        for (i = 0; i < DArCDJ_DCT_SIZE_2D; i++)
        {
            ODH_DCT_SLOT1(cdj_ctrl, odh_zigzag_order[i]) =
                (u16)cdj_ctrl->mDCTBuffer[64 + i];
        }

        // DCT
        fdct_fast(&cdj_ctrl->mDCTBuffer[64], pixBlockPtr + DArCDJ_DCT_SIZE_1D,
                  (u32)(DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * 2),
                  cdj_ctrl->mQuantizationTable[0]);

        
        for (i = 0; i < DArCDJ_DCT_SIZE_2D; i++)
        {
            
            ODH_DCT_SLOT0(cdj_ctrl, odh_zigzag_order[i]) =
                (u16)cdj_ctrl->mDCTBuffer[64 + i];
        }
        cdj_ctrl->mDCTBuffer[64] = 0x40004000;

        
        // HuffmanCoderWram((u16 *)cdj_ctrl->mDCTBuffer,
        //                 cdj_ctrl->mHuffmanRequest);
        result = huffmanCoder((u16 *)cdj_ctrl->mDCTBuffer, cdj_ctrl->mHuffmanRequest);

        
        if (result == DArCDJRESULT_ERR_CODE_SIZE)
        {
            return DArCDJRESULT_ERR_CODE_SIZE;
        }

        
        
        idx >>= 1;
        idx += DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * 2 * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_Y];
        pixBlockPtr = &(cdj_ctrl->mImgYCbCrBufferPtr[idx]);

        // DCT
        fdct_fast(&cdj_ctrl->mDCTBuffer[64], pixBlockPtr,
                  (u32)(DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X]),
                  cdj_ctrl->mQuantizationTable[1]);

        
        for (i = 0; i < DArCDJ_DCT_SIZE_2D; i++)
        {
            ODH_DCT_SLOT1(cdj_ctrl, odh_zigzag_order[i]) =
                (u16)cdj_ctrl->mDCTBuffer[64 + i];
        }

        
        
        idx += DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * 2 * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_Y];
        pixBlockPtr = &(cdj_ctrl->mImgYCbCrBufferPtr[idx]);

        // DCT
        fdct_fast(&cdj_ctrl->mDCTBuffer[64], pixBlockPtr,
                  (u32)(DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X]),
                  cdj_ctrl->mQuantizationTable[1]);

        
        for (i = 0; i < DArCDJ_DCT_SIZE_2D; i++)
        {
            
            ODH_DCT_SLOT0(cdj_ctrl, odh_zigzag_order[i]) =
                (u16)cdj_ctrl->mDCTBuffer[64 + i];
        }
        cdj_ctrl->mDCTBuffer[64] = 0x40004000;

        
        // HuffmanCoderWram((u16 *)cdj_ctrl->mDCTBuffer,
        //                 cdj_ctrl->mHuffmanRequest+1);
        result = huffmanCoder((u16 *)cdj_ctrl->mDCTBuffer, cdj_ctrl->mHuffmanRequest + 1);

        
        if (result == DArCDJRESULT_ERR_CODE_SIZE)
        {
            return DArCDJRESULT_ERR_CODE_SIZE;
        }

    }
    else if (sampX == 1 && sampY == 2)
    {
        
        
        idx = cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X] * DArCDJ_DCT_SIZE_1D + cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y] * DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * 2;
        pixBlockPtr = &(cdj_ctrl->mImgYCbCrBufferPtr[idx]);

        // DCT
        fdct_fast(&cdj_ctrl->mDCTBuffer[64], pixBlockPtr,
                  (u32)(DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X]),
                  cdj_ctrl->mQuantizationTable[0]);

        
        for (i = 0; i < DArCDJ_DCT_SIZE_2D; i++)
        {
            ODH_DCT_SLOT1(cdj_ctrl, odh_zigzag_order[i]) =
                (u16)cdj_ctrl->mDCTBuffer[64 + i];
        }

        // DCT
        fdct_fast(&cdj_ctrl->mDCTBuffer[64], pixBlockPtr + DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X],
                  (u32)(DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X]),
                  cdj_ctrl->mQuantizationTable[0]);

        
        for (i = 0; i < DArCDJ_DCT_SIZE_2D; i++)
        {
            
            ODH_DCT_SLOT0(cdj_ctrl, odh_zigzag_order[i]) =
                (u16)cdj_ctrl->mDCTBuffer[64 + i];
        }
        cdj_ctrl->mDCTBuffer[64] = 0x40004000;

        
        // HuffmanCoderWram((u16 *)cdj_ctrl->mDCTBuffer,
        //                 cdj_ctrl->mHuffmanRequest);
        result = huffmanCoder((u16 *)cdj_ctrl->mDCTBuffer, cdj_ctrl->mHuffmanRequest);

        
        if (result == DArCDJRESULT_ERR_CODE_SIZE)
        {
            return DArCDJRESULT_ERR_CODE_SIZE;
        }

        
        
        idx = cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X] * DArCDJ_DCT_SIZE_1D + cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y] * DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] + DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_Y] * 2;
        pixBlockPtr = &(cdj_ctrl->mImgYCbCrBufferPtr[idx]);

        // DCT
        fdct_fast(&cdj_ctrl->mDCTBuffer[64], pixBlockPtr,
                  (u32)(DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X]),
                  cdj_ctrl->mQuantizationTable[1]);

        
        for (i = 0; i < DArCDJ_DCT_SIZE_2D; i++)
        {
            ODH_DCT_SLOT1(cdj_ctrl, odh_zigzag_order[i]) =
                (u16)cdj_ctrl->mDCTBuffer[64 + i];
        }

        
        
        idx += DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_Y] * 2;
        pixBlockPtr = &(cdj_ctrl->mImgYCbCrBufferPtr[idx]);

        // DCT
        fdct_fast(&cdj_ctrl->mDCTBuffer[64], pixBlockPtr,
                  (u32)(DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X]),
                  cdj_ctrl->mQuantizationTable[1]);

        
        for (i = 0; i < DArCDJ_DCT_SIZE_2D; i++)
        {
            
            ODH_DCT_SLOT0(cdj_ctrl, odh_zigzag_order[i]) =
                (u16)cdj_ctrl->mDCTBuffer[64 + i];
        }
        cdj_ctrl->mDCTBuffer[64] = 0x40004000;

        
        // HuffmanCoderWram((u16 *)cdj_ctrl->mDCTBuffer,
        //                 cdj_ctrl->mHuffmanRequest+1);
        result = huffmanCoder((u16 *)cdj_ctrl->mDCTBuffer, cdj_ctrl->mHuffmanRequest + 1);

        
        if (result == DArCDJRESULT_ERR_CODE_SIZE)
        {
            return DArCDJRESULT_ERR_CODE_SIZE;
        }

    }
    else
    {
        
        
        idx = cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X] * DArCDJ_DCT_SIZE_1D * 2 + cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y] * DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * 4;
        pixBlockPtr = &(cdj_ctrl->mImgYCbCrBufferPtr[idx]);

        // DCT
        fdct_fast(&cdj_ctrl->mDCTBuffer[64], pixBlockPtr,
                  (u32)(DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * 2),
                  cdj_ctrl->mQuantizationTable[0]);

        
        for (i = 0; i < DArCDJ_DCT_SIZE_2D; i++)
        {
            ODH_DCT_SLOT1(cdj_ctrl, odh_zigzag_order[i]) =
                (u16)cdj_ctrl->mDCTBuffer[64 + i];
        }

        // DCT
        fdct_fast(&cdj_ctrl->mDCTBuffer[64], pixBlockPtr + DArCDJ_DCT_SIZE_1D,
                  (u32)(DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * 2),
                  cdj_ctrl->mQuantizationTable[0]);

        
        for (i = 0; i < DArCDJ_DCT_SIZE_2D; i++)
        {
            
            ODH_DCT_SLOT0(cdj_ctrl, odh_zigzag_order[i]) =
                (u16)cdj_ctrl->mDCTBuffer[64 + i];
        }
        cdj_ctrl->mDCTBuffer[64] = 0x40004000;

        
        // HuffmanCoderWram((u16 *)cdj_ctrl->mDCTBuffer,
        //                 cdj_ctrl->mHuffmanRequest);
        result = huffmanCoder((u16 *)cdj_ctrl->mDCTBuffer, cdj_ctrl->mHuffmanRequest);

        
        if (result == DArCDJRESULT_ERR_CODE_SIZE)
        {
            return DArCDJRESULT_ERR_CODE_SIZE;
        }

        
        
        // idx = cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X]*DArCDJ_DCT_SIZE_1D*2+cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y]*DArCDJ_DCT_SIZE_2D*cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X]*4+DArCDJ_DCT_SIZE_2D*cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X]*2;
        pixBlockPtr = &(cdj_ctrl->mImgYCbCrBufferPtr[idx + DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * 2]);

        // DCT
        fdct_fast(&cdj_ctrl->mDCTBuffer[64], pixBlockPtr,
                  (u32)(DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * 2),
                  cdj_ctrl->mQuantizationTable[0]);

        
        for (i = 0; i < DArCDJ_DCT_SIZE_2D; i++)
        {
            ODH_DCT_SLOT1(cdj_ctrl, odh_zigzag_order[i]) =
                (u16)cdj_ctrl->mDCTBuffer[64 + i];
        }

        // DCT
        fdct_fast(&cdj_ctrl->mDCTBuffer[64], pixBlockPtr + DArCDJ_DCT_SIZE_1D,
                  (u32)(DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * 2),
                  cdj_ctrl->mQuantizationTable[0]);

        
        for (i = 0; i < DArCDJ_DCT_SIZE_2D; i++)
        {
            
            ODH_DCT_SLOT0(cdj_ctrl, odh_zigzag_order[i]) =
                (u16)cdj_ctrl->mDCTBuffer[64 + i];
        }
        cdj_ctrl->mDCTBuffer[64] = 0x40004000;

        
        // HuffmanCoderWram((u16 *)cdj_ctrl->mDCTBuffer,
        //                 cdj_ctrl->mHuffmanRequest);
        result = huffmanCoder((u16 *)cdj_ctrl->mDCTBuffer, cdj_ctrl->mHuffmanRequest);

        
        if (result == DArCDJRESULT_ERR_CODE_SIZE)
        {
            return DArCDJRESULT_ERR_CODE_SIZE;
        }

        
        
        idx >>= 1;
        idx = cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X] * DArCDJ_DCT_SIZE_1D + cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y] * DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] + DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_Y] * 4;
        pixBlockPtr = &(cdj_ctrl->mImgYCbCrBufferPtr[idx]);

        // DCT
        fdct_fast(&cdj_ctrl->mDCTBuffer[64], pixBlockPtr,
                  (u32)(DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X]),
                  cdj_ctrl->mQuantizationTable[1]);

        
        for (i = 0; i < DArCDJ_DCT_SIZE_2D; i++)
        {
            ODH_DCT_SLOT1(cdj_ctrl, odh_zigzag_order[i]) =
                (u16)cdj_ctrl->mDCTBuffer[64 + i];
        }

        
        
        idx += DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_Y] * 4;
        pixBlockPtr = &(cdj_ctrl->mImgYCbCrBufferPtr[idx]);

        // DCT
        fdct_fast(&cdj_ctrl->mDCTBuffer[64], pixBlockPtr,
                  (u32)(DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X]),
                  cdj_ctrl->mQuantizationTable[1]);

        
        for (i = 0; i < DArCDJ_DCT_SIZE_2D; i++)
        {
            
            ODH_DCT_SLOT0(cdj_ctrl, odh_zigzag_order[i]) =
                (u16)cdj_ctrl->mDCTBuffer[64 + i];
        }
        cdj_ctrl->mDCTBuffer[64] = 0x40004000;

        
        // HuffmanCoderWram((u16 *)cdj_ctrl->mDCTBuffer,
        //                 cdj_ctrl->mHuffmanRequest+1);
        result = huffmanCoder((u16 *)cdj_ctrl->mDCTBuffer, cdj_ctrl->mHuffmanRequest + 1);

        
        if (result == DArCDJRESULT_ERR_CODE_SIZE)
        {
            return DArCDJRESULT_ERR_CODE_SIZE;
        }
    }

    
        cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X]++;
        if (cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X] == cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X])
        {
            cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y]++;
            cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X] = 0;
        }
    } while (cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y] < cdj_ctrl->mMCUinImage[DArCDJ_AXIS_Y]);

    
    result = cdj_c_flashBuffer(cdj_ctrl);
    
    if (result == DArCDJRESULT_ERR_CODE_SIZE)
    {
        return DArCDJRESULT_ERR_CODE_SIZE;
    }

    
    cdj_c_makeHeader(cdj_ctrl, cdj_ctrl->mCodeBufferSize - cdj_ctrl->mRemainCodeBuffer);

    
    return (cdj_ctrl->mCodeBufferSize - cdj_ctrl->mRemainCodeBuffer);
}

/********************************************************************/

/********************************************************************/
static u32 cdj_c_flashBuffer(SArCDJ_OdhMaster *cdj_ctrl)
{

    
    
    cdj_ctrl->mHuffmanBuffer |= (0x7F << (cdj_ctrl->mHuffmanBits - 7));
    cdj_ctrl->mHuffmanBits -= 7;
    
    while (cdj_ctrl->mHuffmanBits <= 24)
    {
        if (cdj_ctrl->mRemainCodeBuffer == 0)
        {
            
            return DArCDJRESULT_ERR_CODE_SIZE;
        }

        (cdj_ctrl->mCodeBufferPtr)[cdj_ctrl->mCodeBufferSize - cdj_ctrl->mRemainCodeBuffer] =
            (u8)(cdj_ctrl->mHuffmanBuffer >> 24);
        cdj_ctrl->mRemainCodeBuffer--;
        cdj_ctrl->mHuffmanBits += 8;
        cdj_ctrl->mHuffmanBuffer <<= 8;
    }

    
    while ((cdj_ctrl->mCodeBufferSize - cdj_ctrl->mRemainCodeBuffer) & 0x3)
    {
        (cdj_ctrl->mCodeBufferPtr)[cdj_ctrl->mCodeBufferSize - cdj_ctrl->mRemainCodeBuffer] = 0xff;
        cdj_ctrl->mRemainCodeBuffer--;
    }

    return 0;
}

/********************************************************************/

/********************************************************************/

static void cdj_c_setQuantizationTable(SArCDJ_OdhMaster *cdj_ctrl, u32 quality)
{
    u32 cnt, temp, value, currentTable;
    u8 tempQuantBuffer[DArCDJ_QUANTIZE_TABLE_NUM * DArCDJ_DCT_SIZE_2D];

    
    for (currentTable = 0; currentTable < DArCDJ_QUANTIZE_TABLE_NUM; currentTable++)
    {
        for (cnt = 0; cnt < DArCDJ_DCT_SIZE_2D; cnt++)
        {
            temp = (u32)((gArCdj_std_quant_tbl[currentTable][cnt] * quality + 50L) / 100L);
            if (temp <= 0L)
                temp = 1L;
            if (temp > 255L)
                temp = 255L;
            
            tempQuantBuffer[cnt + currentTable * 64] = (u8)temp;
        }
    }

    
    for (currentTable = 0; currentTable < DArCDJ_QUANTIZE_TABLE_NUM; currentTable++)
    {
        for (cnt = 0; cnt < DArCDJ_DCT_SIZE_2D; cnt++)
        {
            value = gArAANScales[cnt];
            value *= tempQuantBuffer[cnt + currentTable * 64];
            value = (1 << (14 + 14 - 2)) / (value);
            cdj_ctrl->mQuantizationTable[currentTable][cnt] = value;
        }
    }
}

/********************************************************************/

/********************************************************************/

static void cdj_c_makeHeader(SArCDJ_OdhMaster *cdj_ctrl, u32 fileSize)
{
    
    
    cdj_ctrl->mCodeBufferPtr[0] = 'A';
    cdj_ctrl->mCodeBufferPtr[1] = 'J';
    cdj_ctrl->mCodeBufferPtr[2] = 'P';
    cdj_ctrl->mCodeBufferPtr[3] = 'G';
    
    AV_WB32(&cdj_ctrl->mCodeBufferPtr[4],
        (u32)(cdj_ctrl->mImageSize[DArCDJ_AXIS_X] | (cdj_ctrl->mImageSize[DArCDJ_AXIS_Y] << 11) | ((cdj_ctrl->mSmpRate[DArCDJ_AXIS_X] - 1) << 22) |
              ((cdj_ctrl->mSmpRate[DArCDJ_AXIS_Y] - 1) << 23) | (cdj_ctrl->mQuality << 24)));
    
    AV_WB32(&cdj_ctrl->mCodeBufferPtr[8], fileSize);
    
    AV_WB32(&cdj_ctrl->mCodeBufferPtr[12], 0);
}

/********************************************************************/

static void fdct_fast(u32 *dctBlock, u8 *pixelBuffer, u32 pixelOffset, u32 *quantTable)
{
    int tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7;
    int tmp10, tmp11, tmp12, tmp13;
    int z1, z2, z3, z4, z5, z11, z13;
    int ctr;
    int *dataptr;
    int i, j;

    
    for (i = 0; i < DArCDJ_DCT_SIZE_1D; i++)
    {
        for (j = 0; j < DArCDJ_DCT_SIZE_1D; j++)
        {
            dctBlock[i + j * DArCDJ_DCT_SIZE_1D] = pixelBuffer[i + j * pixelOffset];
            dctBlock[i + j * DArCDJ_DCT_SIZE_1D] -= 128;
        }
    }

    /* Pass 1: process rows. */

    dataptr = (int *)dctBlock;
    for (ctr = DArCDJ_DCT_SIZE_1D; ctr > 0; ctr--)
    {
        tmp0 = dataptr[0] + dataptr[7];
        tmp7 = dataptr[0] - dataptr[7];
        tmp1 = dataptr[1] + dataptr[6];
        tmp6 = dataptr[1] - dataptr[6];
        tmp2 = dataptr[2] + dataptr[5];
        tmp5 = dataptr[2] - dataptr[5];
        tmp3 = dataptr[3] + dataptr[4];
        tmp4 = dataptr[3] - dataptr[4];

        /* Even part */

        tmp10 = tmp0 + tmp3; /* phase 2 */
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        dataptr[0] = tmp10 + tmp11; /* phase 3 */
        dataptr[4] = tmp10 - tmp11;

        

        z1 = DArMULTIPLY(tmp12 + tmp13, DAr_FIX_0_707106781); /* c4 */
        dataptr[2] = tmp13 + z1;                              /* phase 5 */
        dataptr[6] = tmp13 - z1;

        /* Odd part */

        tmp10 = tmp4 + tmp5; /* phase 2 */
        tmp11 = tmp5 + tmp6;
        tmp12 = tmp6 + tmp7;

        /* The rotator is modified from fig 4-8 to avoid extra negations. */
        z5 = DArMULTIPLY(tmp10 - tmp12, DAr_FIX_0_382683433); /* c6 */
        z2 = DArMULTIPLY(tmp10, DAr_FIX_0_541196100) + z5;    /* c2-c6 */
        z4 = DArMULTIPLY(tmp12, DAr_FIX_1_306562965) + z5;    /* c2+c6 */
        z3 = DArMULTIPLY(tmp11, DAr_FIX_0_707106781);         /* c4 */

        z11 = tmp7 + z3; /* phase 5 */
        z13 = tmp7 - z3;

        dataptr[5] = z13 + z2; /* phase 6 */
        dataptr[3] = z13 - z2;
        dataptr[1] = z11 + z4;
        dataptr[7] = z11 - z4;

        dataptr += DArCDJ_DCT_SIZE_1D; /* advance pointer to next row */
    }

    /* Pass 2: process columns. */

    dataptr = (int *)dctBlock;
    for (ctr = DArCDJ_DCT_SIZE_1D; ctr > 0; ctr--)
    {
        tmp0 = dataptr[DArCDJ_DCT_SIZE_1D * 0] + dataptr[DArCDJ_DCT_SIZE_1D * 7];
        tmp7 = dataptr[DArCDJ_DCT_SIZE_1D * 0] - dataptr[DArCDJ_DCT_SIZE_1D * 7];
        tmp1 = dataptr[DArCDJ_DCT_SIZE_1D * 1] + dataptr[DArCDJ_DCT_SIZE_1D * 6];
        tmp6 = dataptr[DArCDJ_DCT_SIZE_1D * 1] - dataptr[DArCDJ_DCT_SIZE_1D * 6];
        tmp2 = dataptr[DArCDJ_DCT_SIZE_1D * 2] + dataptr[DArCDJ_DCT_SIZE_1D * 5];
        tmp5 = dataptr[DArCDJ_DCT_SIZE_1D * 2] - dataptr[DArCDJ_DCT_SIZE_1D * 5];
        tmp3 = dataptr[DArCDJ_DCT_SIZE_1D * 3] + dataptr[DArCDJ_DCT_SIZE_1D * 4];
        tmp4 = dataptr[DArCDJ_DCT_SIZE_1D * 3] - dataptr[DArCDJ_DCT_SIZE_1D * 4];

        /* Even part */

        tmp10 = tmp0 + tmp3; /* phase 2 */
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        dataptr[DArCDJ_DCT_SIZE_1D * 0] = tmp10 + tmp11; /* phase 3 */
        dataptr[DArCDJ_DCT_SIZE_1D * 4] = tmp10 - tmp11;

        z1 = DArMULTIPLY(tmp12 + tmp13, DAr_FIX_0_707106781); /* c4 */
        dataptr[DArCDJ_DCT_SIZE_1D * 2] = tmp13 + z1;         /* phase 5 */
        dataptr[DArCDJ_DCT_SIZE_1D * 6] = tmp13 - z1;

        /* Odd part */

        tmp10 = tmp4 + tmp5; /* phase 2 */
        tmp11 = tmp5 + tmp6;
        tmp12 = tmp6 + tmp7;

        /* The rotator is modified from fig 4-8 to avoid extra negations. */
        z5 = DArMULTIPLY(tmp10 - tmp12, DAr_FIX_0_382683433); /* c6 */
        z2 = DArMULTIPLY(tmp10, DAr_FIX_0_541196100) + z5;    /* c2-c6 */
        z4 = DArMULTIPLY(tmp12, DAr_FIX_1_306562965) + z5;    /* c2+c6 */
        z3 = DArMULTIPLY(tmp11, DAr_FIX_0_707106781);         /* c4 */

        z11 = tmp7 + z3; /* phase 5 */
        z13 = tmp7 - z3;

        dataptr[DArCDJ_DCT_SIZE_1D * 5] = z13 + z2; /* phase 6 */
        dataptr[DArCDJ_DCT_SIZE_1D * 3] = z13 - z2;
        dataptr[DArCDJ_DCT_SIZE_1D * 1] = z11 + z4;
        dataptr[DArCDJ_DCT_SIZE_1D * 7] = z11 - z4;

        dataptr++; /* advance pointer to next column */
    }

    
    for (i = 0; i < DArCDJ_DCT_SIZE_1D; i++)
    {
        for (j = 0; j < DArCDJ_DCT_SIZE_1D; j++)
        {
            
            

            
            
            // dctBlock[i+j*DArCDJ_DCT_SIZE_1D] <<= 1;
            // if (dctBlock[i+j*DArCDJ_DCT_SIZE_1D] < 0){
            //    dctBlock[i+j*DArCDJ_DCT_SIZE_1D] += 1;
            //}

            
            dctBlock[i + j * DArCDJ_DCT_SIZE_1D] =
                (u32)(((int)(dctBlock[i + j * DArCDJ_DCT_SIZE_1D] * quantTable[i + j * DArCDJ_DCT_SIZE_1D] + 0x4000)) >> 15);
        }
    }
}

/********************************************************************/

/********************************************************************/
static av_always_inline u32 EmitBit(s32 code, s32 len, SArCDJ_HuffmanRequest *huffRequest)
{
    s32 shift;

    shift = (s32)(*huffRequest->mHuffmanBufferRemain - len);
    *huffRequest->mHuffmanBuffer |= (code << shift); 
    *huffRequest->mHuffmanBufferRemain -= len;       

    while (*huffRequest->mHuffmanBufferRemain <= 24)
    {
        
        
        if (*huffRequest->mCodeBufferRemain == 0)
        {
            
            return DArCDJRESULT_ERR_CODE_SIZE;
        }

        huffRequest->mCodeBuffer[huffRequest->mCodeBufferSize -
                                 *huffRequest->mCodeBufferRemain] =
            (u8)(*huffRequest->mHuffmanBuffer >> 24);
        *huffRequest->mCodeBufferRemain -= 1; 

        *huffRequest->mHuffmanBuffer <<= 8;
        *huffRequest->mHuffmanBufferRemain += 8;
    }

    return 0;
}

/********************************************************************/

/********************************************************************/
static u32 huffmanCoder(u16 *DCTBuffer, SArCDJ_HuffmanRequest *huffRequest)
{
    u16 *DctBufBak;
    s32 dctVal; // DCT?W??
    s32 diffDc; 
    s32 tmpVal;
    s32 idt_bits; 
    s32 add_bits; 
    s32 adds;     
    s32 huffCode; 
    int nb;       
    int run;      
    u32 result;

    

    DctBufBak = DCTBuffer;
    DCTBuffer++; 
    nb = 0;

    while (1)
    {
        
        
        dctVal = (s32)(*DCTBuffer << 16);
        dctVal >>= 16;  
        DCTBuffer += 2; 

        
        diffDc = (s32)(dctVal - (*huffRequest->mPreviousDC[nb]));
        *huffRequest->mPreviousDC[nb] = (u32)dctVal; 
        dctVal = diffDc;

        if (dctVal < 0)
        {
            
            tmpVal = -1 * dctVal;
            dctVal -= 1;
        }
        else
        {
            tmpVal = dctVal;
        }

        
        if (tmpVal == 0)
        {
            
            add_bits = 0;
        }
        else
        {
            for (add_bits = 1; (tmpVal >>= 1) != 0; add_bits++)
                ;
        }

        
        huffCode = (s32)huffRequest->mDCTable[add_bits];
        idt_bits = huffCode >> 24; 
        huffCode &= 0x00ffffff;    

        adds = dctVal & ((1 << add_bits) - 1); 
        
        huffCode = (huffCode << add_bits) | adds;

        result = EmitBit(huffCode, idt_bits + add_bits, huffRequest);
        if (result == DArCDJRESULT_ERR_CODE_SIZE)
        {
            
            return DArCDJRESULT_ERR_CODE_SIZE;
        }

        
        while (1)
        {
            run = 0; 

            while (1)
            {
                
                
                dctVal = (s32)(*DCTBuffer << 16);
                dctVal >>= 16;  
                DCTBuffer += 2; 
                if (dctVal == 0)
                {
                    run++;
                }
                else
                {
                    break;
                }
            }

            
            if (dctVal == 0x4000)
            {
                break;
            }

            
            tmpVal = run;

            
            if (tmpVal == 0)
            {
                
                add_bits = 0;
            }
            else
            {
                for (add_bits = 1; (tmpVal >>= 1) != 0; add_bits++)
                    ;
            }

            
            huffCode = (s32)huffRequest->mACTable[add_bits];
            idt_bits = huffCode >> 24; 
            huffCode &= 0x00ffffff;    

            adds = run & ((1 << add_bits) - 1); 
            
            huffCode = (huffCode << add_bits) | adds;

            result = EmitBit(huffCode, idt_bits + add_bits, huffRequest);
            if (result == DArCDJRESULT_ERR_CODE_SIZE)
            {
                
                return DArCDJRESULT_ERR_CODE_SIZE;
            }

            
            if (dctVal < 0)
            {
                
                tmpVal = -1 * dctVal;
                dctVal -= 1;
            }
            else
            {
                tmpVal = dctVal;
            }

            
            if (tmpVal == 0)
            {
                
                add_bits = 0;
            }
            else
            {
                for (add_bits = 1; (tmpVal >>= 1) != 0; add_bits++)
                    ;
            }

            
            huffCode = (s32)huffRequest->mDCTable[add_bits];
            idt_bits = huffCode >> 24; 
            huffCode &= 0x00ffffff;    

            adds = dctVal & ((1 << add_bits) - 1); 
            
            huffCode = (huffCode << add_bits) | adds;

            result = EmitBit(huffCode, idt_bits + add_bits, huffRequest);
            if (result == DArCDJRESULT_ERR_CODE_SIZE)
            {
                
                return DArCDJRESULT_ERR_CODE_SIZE;
            }
        }

        if (run != 0)
        {
            
            huffCode = (s32)huffRequest->mACTable[7];
            result = EmitBit(huffCode & 0x00ffffff, huffCode >> 24, huffRequest);
            if (result == DArCDJRESULT_ERR_CODE_SIZE)
            {
                
                return DArCDJRESULT_ERR_CODE_SIZE;
            }
        }

        if (((uintptr_t)DCTBuffer) & 0x2)
        {
            
            DCTBuffer = DctBufBak; 
            nb = 1;                
        }
        else
        {
            
            break;
        }
    }

    return 0;
}

/********************************************************************/

/********************************************************************/
static u32 cdj_d_initializeDecompressOdh(
    SArCDJ_OdhMaster *cdj_ctrl, u8 *imgYCbCrBufPtr, u8 *CodeBufPtr,
    u32 CodeBufSize)
{
    u32 quality;

    
    
    if ((CodeBufPtr[0] != 'A') || (CodeBufPtr[1] != 'J') || (CodeBufPtr[2] != 'P') || (CodeBufPtr[3] != 'G'))
    {
        return DArCDJRESULT_ERR_INVALID_DATA;
    }
    
    cdj_ctrl->mImageSize[DArCDJ_AXIS_X] =
        (u16)(AV_RB32(CodeBufPtr + 4) & 0x7ff);
    
    cdj_ctrl->mImageSize[DArCDJ_AXIS_Y] =
        (u16)((AV_RB32(CodeBufPtr + 4) >> 11) & 0x7ff);
    
    cdj_ctrl->mSmpRate[DArCDJ_AXIS_X] =
        (u8)(((AV_RB32(CodeBufPtr + 4) >> 22) & 1) + 1);
    
    cdj_ctrl->mSmpRate[DArCDJ_AXIS_Y] =
        (u8)(((AV_RB32(CodeBufPtr + 4) >> 23) & 1) + 1);
    
    cdj_ctrl->mQuality = (u8)((AV_RB32(CodeBufPtr + 4) >> 24) & 0xff);

    
    if ((!cdj_ctrl->mImageSize[DArCDJ_AXIS_X]) || (cdj_ctrl->mImageSize[DArCDJ_AXIS_X] > DArCDJ_PIXEL_SIZE_MAX_X) || (!cdj_ctrl->mImageSize[DArCDJ_AXIS_Y]) || (cdj_ctrl->mImageSize[DArCDJ_AXIS_Y] > DArCDJ_PIXEL_SIZE_MAX_Y))
    {
        return DArCDJRESULT_ERR_SIZE;
    }

    
    if (cdj_ctrl->mQuality > 100)
    {
        return DArCDJRESULT_ERR_QUALITY;
    }

    
    cdj_ctrl->mImgYCbCrBufferPtr = imgYCbCrBufPtr;
    
    cdj_ctrl->mCodeBufferPtr = CodeBufPtr + DArCDJ_HEADER_SIZE;

    
    cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] =
        (u16)((cdj_ctrl->mImageSize[DArCDJ_AXIS_X] - 1) / (DArCDJ_DCT_SIZE_1D * cdj_ctrl->mSmpRate[DArCDJ_AXIS_X]) + 1);
    cdj_ctrl->mMCUinImage[DArCDJ_AXIS_Y] =
        (u16)((cdj_ctrl->mImageSize[DArCDJ_AXIS_Y] - 1) / (DArCDJ_DCT_SIZE_1D * cdj_ctrl->mSmpRate[DArCDJ_AXIS_Y]) + 1);

    
    cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X] = 0;
    cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y] = 0;

    
    cdj_ctrl->mHuffmanBits = 0;

    
    cdj_ctrl->mPreviousDC[DArCDJ_COLOR_Y] = 0;
    cdj_ctrl->mPreviousDC[DArCDJ_COLOR_Cb] = 0;
    cdj_ctrl->mPreviousDC[DArCDJ_COLOR_Cr] = 0;

    
    cdj_ctrl->mHuffmanRequest[0].mPreviousDC[0] = cdj_ctrl->mPreviousDC;
    cdj_ctrl->mHuffmanRequest[0].mPreviousDC[1] = cdj_ctrl->mPreviousDC;
    cdj_ctrl->mHuffmanRequest[0].mCodeBuffer = cdj_ctrl->mCodeBufferPtr;
    cdj_ctrl->mHuffmanRequest[0].mCodeBufferEnd = CodeBufPtr + CodeBufSize;
    cdj_ctrl->mHuffmanRequest[0].mHuffmanBufferRemain = &cdj_ctrl->mHuffmanBits;
    cdj_ctrl->mHuffmanRequest[1].mPreviousDC[0] = cdj_ctrl->mPreviousDC + 1;
    cdj_ctrl->mHuffmanRequest[1].mPreviousDC[1] = cdj_ctrl->mPreviousDC + 2;
    cdj_ctrl->mHuffmanRequest[1].mCodeBuffer = cdj_ctrl->mCodeBufferPtr;
    cdj_ctrl->mHuffmanRequest[1].mCodeBufferEnd = CodeBufPtr + CodeBufSize;
    cdj_ctrl->mHuffmanRequest[1].mHuffmanBufferRemain = &cdj_ctrl->mHuffmanBits;

    
    quality = (u32)cdj_ctrl->mQuality;
    if (quality <= 0)
        quality = 1;
    if (quality < 50)
        quality = (u32)(5000 / quality);
    else
        quality = (u32)(200 - quality * 2);

    cdj_d_setDequantizationTable(cdj_ctrl, quality);

    return DArCDJRESULT_SUCCESS;
}

/********************************************************************/

/********************************************************************/
static u32 cdj_d_decompressLoop(SArCDJ_OdhMaster *cdj_ctrl)
{
    u32 result;
    int idx, i;

    do
    {
        if ((cdj_ctrl->mSmpRate[DArCDJ_AXIS_X] == 1) && (cdj_ctrl->mSmpRate[DArCDJ_AXIS_Y] == 1))
        {
            
            
            result = huffmanDecoder(cdj_ctrl->mDCTBuffer,
                                    cdj_ctrl->mHuffmanRequest,
                                    hufftreePtr, DArCDJ_COLOR_Y);

            
            
            (cdj_ctrl->mHuffmanRequest[1]).mCodeBuffer =
                (cdj_ctrl->mHuffmanRequest[0]).mCodeBuffer;

            if (result)
                return result; 

            
            for (i = 0; i < 64; i++)
            {
                cdj_ctrl->mDCTBuffer[odh_natural_order[i] + 64] =
                    cdj_ctrl->mDCTBuffer[i];
            }

            
            idx = DArCDJ_DCT_SIZE_1D * cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X] + DArCDJ_DCT_SIZE_2D * cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y] * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X];

            
            idct_fast(range_limit, cdj_ctrl->mQuantizationTable[0],
                      &(cdj_ctrl->mDCTBuffer[64]),
                      &(cdj_ctrl->mImgYCbCrBufferPtr[idx]),
                      (u32)DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X]);
            
            
            result = huffmanDecoder(cdj_ctrl->mDCTBuffer,
                                    cdj_ctrl->mHuffmanRequest + 1,
                                    &hufftreePtr[2], DArCDJ_COLOR_Cb);

            if (result)
                return result; 

            
            for (i = 0; i < 64; i++)
            {
                cdj_ctrl->mDCTBuffer[odh_natural_order[i] + 64] =
                    cdj_ctrl->mDCTBuffer[i];
            }

            
            idx = DArCDJ_DCT_SIZE_1D * cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X] + DArCDJ_DCT_SIZE_2D * cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y] * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] + DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_Y];

            
            idct_fast(range_limit, cdj_ctrl->mQuantizationTable[1],
                      &(cdj_ctrl->mDCTBuffer[64]),
                      &(cdj_ctrl->mImgYCbCrBufferPtr[idx]),
                      (u32)DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X]);

            
            
            result = huffmanDecoder(cdj_ctrl->mDCTBuffer,
                                    cdj_ctrl->mHuffmanRequest + 1,
                                    &hufftreePtr[2], DArCDJ_COLOR_Cr);

            
            
            (cdj_ctrl->mHuffmanRequest[0]).mCodeBuffer =
                (cdj_ctrl->mHuffmanRequest[1]).mCodeBuffer;

            if (result)
                return result; 

            
            for (i = 0; i < 64; i++)
            {
                cdj_ctrl->mDCTBuffer[odh_natural_order[i] + 64] =
                    cdj_ctrl->mDCTBuffer[i];
            }

            
            idx = DArCDJ_DCT_SIZE_1D * cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X] + DArCDJ_DCT_SIZE_2D * cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y] * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] + DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_Y] * 2;

            
            idct_fast(range_limit, cdj_ctrl->mQuantizationTable[1],
                      &(cdj_ctrl->mDCTBuffer[64]),
                      &(cdj_ctrl->mImgYCbCrBufferPtr[idx]),
                      (u32)DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X]);
        }
        else if ((cdj_ctrl->mSmpRate[DArCDJ_AXIS_X] == 2) && (cdj_ctrl->mSmpRate[DArCDJ_AXIS_Y] == 1))
        {
            
            
            result = huffmanDecoder(cdj_ctrl->mDCTBuffer,
                                    cdj_ctrl->mHuffmanRequest,
                                    hufftreePtr, DArCDJ_COLOR_Y);

            if (result)
                return result; 

            
            for (i = 0; i < 64; i++)
            {
                cdj_ctrl->mDCTBuffer[odh_natural_order[i] + 64] =
                    cdj_ctrl->mDCTBuffer[i];
            }

            
            idx = DArCDJ_DCT_SIZE_1D * cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X] * 2 + DArCDJ_DCT_SIZE_2D * cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y] * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * 2;

            
            idct_fast(range_limit, cdj_ctrl->mQuantizationTable[0],
                      &(cdj_ctrl->mDCTBuffer[64]),
                      &(cdj_ctrl->mImgYCbCrBufferPtr[idx]),
                      (u32)DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * 2);

            
            
            result = huffmanDecoder(cdj_ctrl->mDCTBuffer,
                                    cdj_ctrl->mHuffmanRequest,
                                    hufftreePtr, DArCDJ_COLOR_Y);

            
            
            (cdj_ctrl->mHuffmanRequest[1]).mCodeBuffer =
                (cdj_ctrl->mHuffmanRequest[0]).mCodeBuffer;

            if (result)
                return result; 

            
            for (i = 0; i < 64; i++)
            {
                cdj_ctrl->mDCTBuffer[odh_natural_order[i] + 64] =
                    cdj_ctrl->mDCTBuffer[i];
            }

            
            idx += DArCDJ_DCT_SIZE_1D;

            
            idct_fast(range_limit, cdj_ctrl->mQuantizationTable[0],
                      &(cdj_ctrl->mDCTBuffer[64]),
                      &(cdj_ctrl->mImgYCbCrBufferPtr[idx]),
                      (u32)DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * 2);

            
            
            result = huffmanDecoder(cdj_ctrl->mDCTBuffer,
                                    cdj_ctrl->mHuffmanRequest + 1,
                                    &hufftreePtr[2], DArCDJ_COLOR_Cb);

            if (result)
                return result; 

            
            for (i = 0; i < 64; i++)
            {
                cdj_ctrl->mDCTBuffer[odh_natural_order[i] + 64] =
                    cdj_ctrl->mDCTBuffer[i];
            }

            
            idx = DArCDJ_DCT_SIZE_1D * cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X] + DArCDJ_DCT_SIZE_2D * cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y] * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] + DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * 2 * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_Y];

            
            idct_fast(range_limit, cdj_ctrl->mQuantizationTable[1],
                      &(cdj_ctrl->mDCTBuffer[64]),
                      &(cdj_ctrl->mImgYCbCrBufferPtr[idx]),
                      (u32)DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X]);

            
            
            result = huffmanDecoder(cdj_ctrl->mDCTBuffer,
                                    cdj_ctrl->mHuffmanRequest + 1,
                                    &hufftreePtr[2], DArCDJ_COLOR_Cr);

            
            
            (cdj_ctrl->mHuffmanRequest[0]).mCodeBuffer =
                (cdj_ctrl->mHuffmanRequest[1]).mCodeBuffer;

            if (result)
                return result; 

            
            for (i = 0; i < 64; i++)
            {
                cdj_ctrl->mDCTBuffer[odh_natural_order[i] + 64] =
                    cdj_ctrl->mDCTBuffer[i];
            }

            
            idx = DArCDJ_DCT_SIZE_1D * cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X] + DArCDJ_DCT_SIZE_2D * cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y] * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] + DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * 2 * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_Y] * 2;

            
            idct_fast(range_limit, cdj_ctrl->mQuantizationTable[1],
                      &(cdj_ctrl->mDCTBuffer[64]),
                      &(cdj_ctrl->mImgYCbCrBufferPtr[idx]),
                      (u32)DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X]);
        }
        else if ((cdj_ctrl->mSmpRate[DArCDJ_AXIS_X] == 1) && (cdj_ctrl->mSmpRate[DArCDJ_AXIS_Y] == 2))
        {
            
            
            result = huffmanDecoder(cdj_ctrl->mDCTBuffer,
                                    cdj_ctrl->mHuffmanRequest,
                                    hufftreePtr, DArCDJ_COLOR_Y);

            if (result)
                return result; 

            
            for (i = 0; i < 64; i++)
            {
                cdj_ctrl->mDCTBuffer[odh_natural_order[i] + 64] =
                    cdj_ctrl->mDCTBuffer[i];
            }

            
            idx = DArCDJ_DCT_SIZE_1D * cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X] + DArCDJ_DCT_SIZE_2D * cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y] * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * 2;

            
            idct_fast(range_limit, cdj_ctrl->mQuantizationTable[0],
                      &(cdj_ctrl->mDCTBuffer[64]),
                      &(cdj_ctrl->mImgYCbCrBufferPtr[idx]),
                      (u32)DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X]);

            
            
            result = huffmanDecoder(cdj_ctrl->mDCTBuffer,
                                    cdj_ctrl->mHuffmanRequest,
                                    hufftreePtr, DArCDJ_COLOR_Y);

            
            
            (cdj_ctrl->mHuffmanRequest[1]).mCodeBuffer =
                (cdj_ctrl->mHuffmanRequest[0]).mCodeBuffer;

            if (result)
                return result; 

            
            for (i = 0; i < 64; i++)
            {
                cdj_ctrl->mDCTBuffer[odh_natural_order[i] + 64] =
                    cdj_ctrl->mDCTBuffer[i];
            }

            
            idx += DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X];

            
            idct_fast(range_limit, cdj_ctrl->mQuantizationTable[0],
                      &(cdj_ctrl->mDCTBuffer[64]),
                      &(cdj_ctrl->mImgYCbCrBufferPtr[idx]),
                      (u32)DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X]);

            
            
            result = huffmanDecoder(cdj_ctrl->mDCTBuffer,
                                    cdj_ctrl->mHuffmanRequest + 1,
                                    &hufftreePtr[2], DArCDJ_COLOR_Cb);

            if (result)
                return result; 

            
            for (i = 0; i < 64; i++)
            {
                cdj_ctrl->mDCTBuffer[odh_natural_order[i] + 64] =
                    cdj_ctrl->mDCTBuffer[i];
            }

            
            idx = DArCDJ_DCT_SIZE_1D * cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X] + DArCDJ_DCT_SIZE_2D * cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y] * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] + DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_Y] * 2;

            
            idct_fast(range_limit, cdj_ctrl->mQuantizationTable[1],
                      &(cdj_ctrl->mDCTBuffer[64]),
                      &(cdj_ctrl->mImgYCbCrBufferPtr[idx]),
                      (u32)DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X]);

            
            
            result = huffmanDecoder(cdj_ctrl->mDCTBuffer,
                                    cdj_ctrl->mHuffmanRequest + 1,
                                    &hufftreePtr[2], DArCDJ_COLOR_Cr);

            
            
            (cdj_ctrl->mHuffmanRequest[0]).mCodeBuffer =
                (cdj_ctrl->mHuffmanRequest[1]).mCodeBuffer;

            if (result)
                return result; 

            
            for (i = 0; i < 64; i++)
            {
                cdj_ctrl->mDCTBuffer[odh_natural_order[i] + 64] =
                    cdj_ctrl->mDCTBuffer[i];
            }

            
            idx = DArCDJ_DCT_SIZE_1D * cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X] + DArCDJ_DCT_SIZE_2D * cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y] * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] + DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_Y] * 4;

            
            idct_fast(range_limit, cdj_ctrl->mQuantizationTable[1],
                      &(cdj_ctrl->mDCTBuffer[64]),
                      &(cdj_ctrl->mImgYCbCrBufferPtr[idx]),
                      (u32)DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X]);
        }
        else if ((cdj_ctrl->mSmpRate[DArCDJ_AXIS_X] == 2) && (cdj_ctrl->mSmpRate[DArCDJ_AXIS_Y] == 2))
        {
            
            
            result = huffmanDecoder(cdj_ctrl->mDCTBuffer,
                                    cdj_ctrl->mHuffmanRequest,
                                    hufftreePtr, DArCDJ_COLOR_Y);

            if (result)
                return result; 

            
            for (i = 0; i < 64; i++)
            {
                cdj_ctrl->mDCTBuffer[odh_natural_order[i] + 64] =
                    cdj_ctrl->mDCTBuffer[i];
            }

            
            idx = DArCDJ_DCT_SIZE_1D * cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X] * 2 + DArCDJ_DCT_SIZE_2D * cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y] * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * 4;

            
            idct_fast(range_limit, cdj_ctrl->mQuantizationTable[0],
                      &(cdj_ctrl->mDCTBuffer[64]),
                      &(cdj_ctrl->mImgYCbCrBufferPtr[idx]),
                      (u32)DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * 2);

            
            
            result = huffmanDecoder(cdj_ctrl->mDCTBuffer,
                                    cdj_ctrl->mHuffmanRequest,
                                    hufftreePtr, DArCDJ_COLOR_Y);

            if (result)
                return result; 

            
            for (i = 0; i < 64; i++)
            {
                cdj_ctrl->mDCTBuffer[odh_natural_order[i] + 64] =
                    cdj_ctrl->mDCTBuffer[i];
            }

            
            idx += DArCDJ_DCT_SIZE_1D;

            
            idct_fast(range_limit, cdj_ctrl->mQuantizationTable[0],
                      &(cdj_ctrl->mDCTBuffer[64]),
                      &(cdj_ctrl->mImgYCbCrBufferPtr[idx]),
                      (u32)DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * 2);

            
            
            result = huffmanDecoder(cdj_ctrl->mDCTBuffer,
                                    cdj_ctrl->mHuffmanRequest,
                                    hufftreePtr, DArCDJ_COLOR_Y);

            if (result)
                return result; 

            
            for (i = 0; i < 64; i++)
            {
                cdj_ctrl->mDCTBuffer[odh_natural_order[i] + 64] =
                    cdj_ctrl->mDCTBuffer[i];
            }

            
            idx += DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * 2 - DArCDJ_DCT_SIZE_1D;

            
            idct_fast(range_limit, cdj_ctrl->mQuantizationTable[0],
                      &(cdj_ctrl->mDCTBuffer[64]),
                      &(cdj_ctrl->mImgYCbCrBufferPtr[idx]),
                      (u32)DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * 2);

            
            
            result = huffmanDecoder(cdj_ctrl->mDCTBuffer,
                                    cdj_ctrl->mHuffmanRequest,
                                    hufftreePtr, DArCDJ_COLOR_Y);

            
            
            (cdj_ctrl->mHuffmanRequest[1]).mCodeBuffer =
                (cdj_ctrl->mHuffmanRequest[0]).mCodeBuffer;

            if (result)
                return result; 

            
            for (i = 0; i < 64; i++)
            {
                cdj_ctrl->mDCTBuffer[odh_natural_order[i] + 64] =
                    cdj_ctrl->mDCTBuffer[i];
            }

            
            idx += DArCDJ_DCT_SIZE_1D;

            
            idct_fast(range_limit, cdj_ctrl->mQuantizationTable[0],
                      &(cdj_ctrl->mDCTBuffer[64]),
                      &(cdj_ctrl->mImgYCbCrBufferPtr[idx]),
                      (u32)DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * 2);

            
            
            result = huffmanDecoder(cdj_ctrl->mDCTBuffer,
                                    cdj_ctrl->mHuffmanRequest + 1,
                                    &hufftreePtr[2], DArCDJ_COLOR_Cb);

            if (result)
                return result; 

            
            for (i = 0; i < 64; i++)
            {
                cdj_ctrl->mDCTBuffer[odh_natural_order[i] + 64] =
                    cdj_ctrl->mDCTBuffer[i];
            }

            
            idx = DArCDJ_DCT_SIZE_1D * cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X] + DArCDJ_DCT_SIZE_2D * cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y] * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] + DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_Y] * 4;

            
            idct_fast(range_limit, cdj_ctrl->mQuantizationTable[1],
                      &(cdj_ctrl->mDCTBuffer[64]),
                      &(cdj_ctrl->mImgYCbCrBufferPtr[idx]),
                      (u32)DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X]);

            
            
            result = huffmanDecoder(cdj_ctrl->mDCTBuffer,
                                    cdj_ctrl->mHuffmanRequest + 1,
                                    &hufftreePtr[2], DArCDJ_COLOR_Cr);

            
            
            (cdj_ctrl->mHuffmanRequest[0]).mCodeBuffer =
                (cdj_ctrl->mHuffmanRequest[1]).mCodeBuffer;

            if (result)
                return result; 

            
            for (i = 0; i < 64; i++)
            {
                cdj_ctrl->mDCTBuffer[odh_natural_order[i] + 64] =
                    cdj_ctrl->mDCTBuffer[i];
            }

            
            idx = DArCDJ_DCT_SIZE_1D * cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X] + DArCDJ_DCT_SIZE_2D * cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y] * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] + DArCDJ_DCT_SIZE_2D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X] * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_Y] * 8;

            
            idct_fast(range_limit, cdj_ctrl->mQuantizationTable[1],
                      &(cdj_ctrl->mDCTBuffer[64]),
                      &(cdj_ctrl->mImgYCbCrBufferPtr[idx]),
                      (u32)DArCDJ_DCT_SIZE_1D * cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X]);
        }
        else
        {
            
        }

        
        cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X]++;
        if (cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X] == cdj_ctrl->mMCUinImage[DArCDJ_AXIS_X])
        {
            cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y]++;
            cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_X] = 0;
        }
    } while (cdj_ctrl->mCurrentMCU[DArCDJ_AXIS_Y] < cdj_ctrl->mMCUinImage[DArCDJ_AXIS_Y]);

    return DArCDJRESULT_SUCCESS;
}

/********************************************************************/

/********************************************************************/

static void cdj_d_setDequantizationTable(SArCDJ_OdhMaster *cdj_ctrl, u32 quality)
{
    u32 cnt, temp, currentTable;

    
    for (currentTable = 0; currentTable < DArCDJ_QUANTIZE_TABLE_NUM; currentTable++)
    {
        for (cnt = 0; cnt < DArCDJ_DCT_SIZE_2D; cnt++)
        {
            temp = (gArCdj_std_quant_tbl[currentTable][cnt] * quality + 50L) / 100L;
            if (temp <= 0L)
                temp = 1L;
            if (temp > 255L)
                temp = 255L;

            cdj_ctrl->mQuantizationTable[currentTable][cnt] =
                (u32)((temp * gArAANScales[cnt] + 2048) >> 12);
        }
    }
}

/********************************************************************/

/********************************************************************/
/*
 * The entropy decoder always works from a 32-bit window, so the last few
 * blocks of a stream legitimately peek past the final coded byte.  Read those
 * as zero rather than off the end of the packet; genuinely malformed data
 * still trips the huffman-code validity checks below.
 */
static av_always_inline u32 odh_peek32(const u8 *p, const u8 *end)
{
    u32 v = 0;
    int i;

    for (i = 0; i < 4; i++)
        v = (v << 8) | (p + i < end ? p[i] : 0);

    return v;
}

static u32 huffmanDecoder(u32 *DCTBlock, SArCDJ_HuffmanRequest *huffmanRequest,
                          const u16 *const *hufftree, int col)
{
    u8 *bufPtr;   
    u32 buf_ofs;  
                  
    u32 code;     
    u32 bit_mask; 
    
    int blk_idx;         
    int idt_bits;        
    int add_bits;        
    int treeIdx;         
    int nextOfs;         
    int idt_bits_max_DC; 
    int i, colBak;

    blk_idx = 0;

    
    bufPtr = huffmanRequest->mCodeBuffer;
    buf_ofs = *huffmanRequest->mHuffmanBufferRemain;
    code = odh_peek32(bufPtr, huffmanRequest->mCodeBufferEnd) << buf_ofs;

    if (col == 0)
    {
        
        idt_bits_max_DC = 9;
    }
    else
    {
        
        idt_bits_max_DC = 11;
    }

    
    for (idt_bits = 1, treeIdx = 0; idt_bits <= idt_bits_max_DC; idt_bits++)
    {
        nextOfs = ((hufftree[0])[treeIdx] & 0x3fff);
        if (((code >> (32 - idt_bits)) & 1) == 0)
        {
            
            if ((hufftree[0])[treeIdx] & 0x8000)
            {
                
                add_bits = (hufftree[0])[treeIdx + nextOfs];
                break;
            }
            else
            {
                treeIdx += nextOfs;
            }
        }
        else
        {
            
            if ((hufftree[0])[treeIdx] & 0x4000)
            {
                
                add_bits = (hufftree[0])[treeIdx + nextOfs + 1];
                break;
            }
            else
            {
                treeIdx += nextOfs + 1;
            }
        }
    }

    
    
    if (idt_bits > idt_bits_max_DC)
        return DArCDJRESULT_ERR_HUFFCODE;

    if (add_bits > 0)
    {
        
        bit_mask = (u32)((1 << add_bits) - 1); 
        code = (code >> (32 - idt_bits - add_bits)) & bit_mask;

        
        if ((code & (1 << (add_bits - 1))) == 0)
        {
            code = (code + 1) | ~bit_mask;
        }
    }
    else
    {
        
        code = 0;
    }

    
    
    colBak = col >> 1; 
    DCTBlock[blk_idx] = code + (*huffmanRequest->mPreviousDC[colBak]);
    *huffmanRequest->mPreviousDC[colBak] = DCTBlock[blk_idx];
    blk_idx++;

    
    
    
    buf_ofs += idt_bits + add_bits;
    
    while (buf_ofs >= 8)
    {
        bufPtr++;
        buf_ofs -= 8;
    }

    
    while (1)
    {
        code = odh_peek32(bufPtr, huffmanRequest->mCodeBufferEnd) << buf_ofs;

        
        
        for (idt_bits = 1, treeIdx = 0; idt_bits <= 6; idt_bits++)
        {
            nextOfs = ((hufftree[1])[treeIdx] & 0x3fff);
            if (((code >> (32 - idt_bits)) & 1) == 0)
            {
                
                if ((hufftree[1])[treeIdx] & 0x8000)
                {
                    
                    add_bits = (hufftree[1])[treeIdx + nextOfs];
                    break;
                }
                else
                {
                    treeIdx += nextOfs;
                }
            }
            else
            {
                
                if ((hufftree[1])[treeIdx] & 0x4000)
                {
                    
                    add_bits = (hufftree[1])[treeIdx + nextOfs + 1];
                    break;
                }
                else
                {
                    treeIdx += nextOfs + 1;
                }
            }
        }

        
        
        if (idt_bits > 6)
            return DArCDJRESULT_ERR_HUFFCODE;

        if (add_bits == 7)
        {
            
            
            buf_ofs += idt_bits;
            
            while (buf_ofs >= 8)
            {
                bufPtr++;
                buf_ofs -= 8;
            }
            break;
        }
        else
        {
            if (add_bits > 0)
            {
                
                
                
                bit_mask = (u32)((1 << add_bits) - 1); 
                code = (code >> (32 - idt_bits - add_bits)) & bit_mask;

                
                
                if ((code & (1 << (add_bits - 1))) == 0)
                {
                    // code = (code+1) | ~bit_mask;
                    return DArCDJRESULT_ERR_HUFFCODE;
                }
            }
            else
            {
                
                code = 0;
            }

            
            if (blk_idx + (int)code > ODH_BLOCK_COEFFS)
                return DArCDJRESULT_ERR_INVALID_DATA;

            for (i = 0; i < code; i++)
            {
                DCTBlock[blk_idx] = 0;
                blk_idx++;
            }

            
            
            
            buf_ofs += idt_bits + add_bits;
            
            while (buf_ofs >= 8)
            {
                bufPtr++;
                buf_ofs -= 8;
            }
            code = odh_peek32(bufPtr, huffmanRequest->mCodeBufferEnd) << buf_ofs;

            if (col == 0)
            {
                
                idt_bits_max_DC = 9;
            }
            else
            {
                
                idt_bits_max_DC = 11;
            }

            
            for (idt_bits = 1, treeIdx = 0; idt_bits <= idt_bits_max_DC; idt_bits++)
            {
                nextOfs = ((hufftree[0])[treeIdx] & 0x3fff);
                if (((code >> (32 - idt_bits)) & 1) == 0)
                {
                    
                    if ((hufftree[0])[treeIdx] & 0x8000)
                    {
                        
                        add_bits = (hufftree[0])[treeIdx + nextOfs];
                        break;
                    }
                    else
                    {
                        treeIdx += nextOfs;
                    }
                }
                else
                {
                    
                    if ((hufftree[0])[treeIdx] & 0x4000)
                    {
                        
                        add_bits = (hufftree[0])[treeIdx + nextOfs + 1];
                        break;
                    }
                    else
                    {
                        treeIdx += nextOfs + 1;
                    }
                }
            }

            
            
            if (idt_bits > idt_bits_max_DC)
                return DArCDJRESULT_ERR_HUFFCODE;

            if (add_bits > 0)
            {
                
                bit_mask = (u32)((1 << add_bits) - 1); 
                code = (code >> (32 - idt_bits - add_bits)) & bit_mask;

                
                if ((code & (1 << (add_bits - 1))) == 0)
                {
                    code = (code + 1) | ~bit_mask;
                }
            }
            else
            {
                
                code = 0;
            }

            
            if (blk_idx >= ODH_BLOCK_COEFFS)
                return DArCDJRESULT_ERR_INVALID_DATA;

            DCTBlock[blk_idx] = code;
            blk_idx++;

            
            
            
            buf_ofs += idt_bits + add_bits;
            
            while (buf_ofs >= 8)
            {
                bufPtr++;
                buf_ofs -= 8;
            }

            if (blk_idx >= ODH_BLOCK_COEFFS)
            {
                
                break;
            }
        }
    }

    

    
    for (; blk_idx < ODH_BLOCK_COEFFS; blk_idx++)
    {
        DCTBlock[blk_idx] = 0;
    }

    
    huffmanRequest->mCodeBuffer = bufPtr;
    *(huffmanRequest->mHuffmanBufferRemain) = buf_ofs;

    return DArCDJRESULT_SUCCESS;
}

/********************************************************************/

/********************************************************************/
static void idct_fast(const u8 *range_limit, u32 *quant_table,
                          u32 *DCTBuffer, u8 *output_buf, u32 pixelOffset)
{
    int tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7;
    int tmp10, tmp11, tmp12, tmp13;
    int z5, z10, z11, z12, z13;
    s32 *inptr;
    s32 *quantptr;
    int *wsptr;
    u8 *outptr;
    u8 dcval;
    int dcval_tmp;
    int ctr;
    int workspace[DArCDJ_DCT_SIZE_2D]; /* buffers data between passes */

    /* Pass 1: process columns from input, store into work array. */

    inptr = (s32 *)DCTBuffer;
    quantptr = (s32 *)quant_table;
    wsptr = workspace;
    for (ctr = DArCDJ_DCT_SIZE_1D; ctr > 0; ctr--)
    {

        if ((inptr[DArCDJ_DCT_SIZE_1D * 1] == 0) && (inptr[DArCDJ_DCT_SIZE_1D * 2] == 0) && (inptr[DArCDJ_DCT_SIZE_1D * 3] == 0) && (inptr[DArCDJ_DCT_SIZE_1D * 4] == 0) && (inptr[DArCDJ_DCT_SIZE_1D * 5] == 0) && (inptr[DArCDJ_DCT_SIZE_1D * 6] == 0) && (inptr[DArCDJ_DCT_SIZE_1D * 7] == 0))
        {
            /* AC terms all zero */
            dcval_tmp = inptr[DArCDJ_DCT_SIZE_1D * 0] * quantptr[DArCDJ_DCT_SIZE_1D * 0];

            wsptr[DArCDJ_DCT_SIZE_1D * 0] = dcval_tmp;
            wsptr[DArCDJ_DCT_SIZE_1D * 1] = dcval_tmp;
            wsptr[DArCDJ_DCT_SIZE_1D * 2] = dcval_tmp;
            wsptr[DArCDJ_DCT_SIZE_1D * 3] = dcval_tmp;
            wsptr[DArCDJ_DCT_SIZE_1D * 4] = dcval_tmp;
            wsptr[DArCDJ_DCT_SIZE_1D * 5] = dcval_tmp;
            wsptr[DArCDJ_DCT_SIZE_1D * 6] = dcval_tmp;
            wsptr[DArCDJ_DCT_SIZE_1D * 7] = dcval_tmp;

            inptr++; /* advance pointers to next column */
            quantptr++;
            wsptr++;
            continue;
        }

        /* Even part */

        tmp0 = inptr[DArCDJ_DCT_SIZE_1D * 0] * quantptr[DArCDJ_DCT_SIZE_1D * 0];
        tmp1 = inptr[DArCDJ_DCT_SIZE_1D * 2] * quantptr[DArCDJ_DCT_SIZE_1D * 2];
        tmp2 = inptr[DArCDJ_DCT_SIZE_1D * 4] * quantptr[DArCDJ_DCT_SIZE_1D * 4];
        tmp3 = inptr[DArCDJ_DCT_SIZE_1D * 6] * quantptr[DArCDJ_DCT_SIZE_1D * 6];

        tmp10 = tmp0 + tmp2; /* phase 3 */
        tmp11 = tmp0 - tmp2;

        tmp13 = tmp1 + tmp3;                                          /* phases 5-3 */
        tmp12 = DArMULTIPLY(tmp1 - tmp3, DArFIX_1_414213562) - tmp13; /* 2*c4 */

        tmp0 = tmp10 + tmp13; /* phase 2 */
        tmp3 = tmp10 - tmp13;
        tmp1 = tmp11 + tmp12;
        tmp2 = tmp11 - tmp12;

        /* Odd part */

        tmp4 = inptr[DArCDJ_DCT_SIZE_1D * 1] * quantptr[DArCDJ_DCT_SIZE_1D * 1];
        tmp5 = inptr[DArCDJ_DCT_SIZE_1D * 3] * quantptr[DArCDJ_DCT_SIZE_1D * 3];
        tmp6 = inptr[DArCDJ_DCT_SIZE_1D * 5] * quantptr[DArCDJ_DCT_SIZE_1D * 5];
        tmp7 = inptr[DArCDJ_DCT_SIZE_1D * 7] * quantptr[DArCDJ_DCT_SIZE_1D * 7];

        z13 = tmp6 + tmp5; /* phase 6 */
        z10 = tmp6 - tmp5;
        z11 = tmp4 + tmp7;
        z12 = tmp4 - tmp7;

        tmp7 = z11 + z13;                                   /* phase 5 */
        tmp11 = DArMULTIPLY(z11 - z13, DArFIX_1_414213562); /* 2*c4 */

        z5 = DArMULTIPLY(z10 + z12, DArFIX_1_847759065);    /* 2*c2 */
        tmp10 = DArMULTIPLY(z12, DArFIX_1_082392200) - z5;  /* 2*(c2-c6) */
        tmp12 = DArMULTIPLY(z10, -DArFIX_2_613125930) + z5; /* -2*(c2+c6) */

        tmp6 = tmp12 - tmp7; /* phase 2 */
        tmp5 = tmp11 - tmp6;
        tmp4 = tmp10 + tmp5;

        wsptr[DArCDJ_DCT_SIZE_1D * 0] = (int)(tmp0 + tmp7);
        wsptr[DArCDJ_DCT_SIZE_1D * 7] = (int)(tmp0 - tmp7);
        wsptr[DArCDJ_DCT_SIZE_1D * 1] = (int)(tmp1 + tmp6);
        wsptr[DArCDJ_DCT_SIZE_1D * 6] = (int)(tmp1 - tmp6);
        wsptr[DArCDJ_DCT_SIZE_1D * 2] = (int)(tmp2 + tmp5);
        wsptr[DArCDJ_DCT_SIZE_1D * 5] = (int)(tmp2 - tmp5);
        wsptr[DArCDJ_DCT_SIZE_1D * 4] = (int)(tmp3 + tmp4);
        wsptr[DArCDJ_DCT_SIZE_1D * 3] = (int)(tmp3 - tmp4);

        inptr++; /* advance pointers to next column */
        quantptr++;
        wsptr++;
    }

    /* Pass 2: process rows from work array, store into output array. */
    /* Note that we must descale the results by a factor of 8 == 2**3, */
    /* and also undo the PASS1_BITS scaling. */

    wsptr = workspace;
    for (ctr = 0; ctr < DArCDJ_DCT_SIZE_1D; ctr++)
    {
        outptr = output_buf + ctr * pixelOffset;

        if ((wsptr[1] == 0) && (wsptr[2] == 0) && (wsptr[3] == 0) && (wsptr[4] == 0) && (wsptr[5] == 0) && (wsptr[6] == 0) && (wsptr[7] == 0))
        {
            /* AC terms all zero */
            dcval = range_limit[DArDESCALE(wsptr[0], 5) & DArRANGE_MASK];
            outptr[0] = dcval;
            outptr[1] = dcval;
            outptr[2] = dcval;
            outptr[3] = dcval;
            outptr[4] = dcval;
            outptr[5] = dcval;
            outptr[6] = dcval;
            outptr[7] = dcval;

            wsptr += DArCDJ_DCT_SIZE_1D; /* advance pointer to next row */
            continue;
        }

        /* Even part */

        tmp10 = ((int)wsptr[0] + (int)wsptr[4]);
        tmp11 = ((int)wsptr[0] - (int)wsptr[4]);

        tmp13 = ((int)wsptr[2] + (int)wsptr[6]);
        tmp12 = DArMULTIPLY((int)wsptr[2] - (int)wsptr[6], DArFIX_1_414213562) - tmp13;

        tmp0 = tmp10 + tmp13;
        tmp3 = tmp10 - tmp13;
        tmp1 = tmp11 + tmp12;
        tmp2 = tmp11 - tmp12;

        /* Odd part */

        z13 = (int)wsptr[5] + (int)wsptr[3];
        z10 = (int)wsptr[5] - (int)wsptr[3];
        z11 = (int)wsptr[1] + (int)wsptr[7];
        z12 = (int)wsptr[1] - (int)wsptr[7];

        tmp7 = z11 + z13;                                   /* phase 5 */
        tmp11 = DArMULTIPLY(z11 - z13, DArFIX_1_414213562); /* 2*c4 */

        z5 = DArMULTIPLY(z10 + z12, DArFIX_1_847759065);    /* 2*c2 */
        tmp10 = DArMULTIPLY(z12, DArFIX_1_082392200) - z5;  /* 2*(c2-c6) */
        tmp12 = DArMULTIPLY(z10, -DArFIX_2_613125930) + z5; /* -2*(c2+c6) */

        tmp6 = tmp12 - tmp7; /* phase 2 */
        tmp5 = tmp11 - tmp6;
        tmp4 = tmp10 + tmp5;

        /* Final output stage: scale down by a factor of 8 and range-limit */
        outptr[0] = range_limit[DArDESCALE(tmp0 + tmp7, 5) & DArRANGE_MASK];
        outptr[7] = range_limit[DArDESCALE(tmp0 - tmp7, 5) & DArRANGE_MASK];
        outptr[1] = range_limit[DArDESCALE(tmp1 + tmp6, 5) & DArRANGE_MASK];
        outptr[6] = range_limit[DArDESCALE(tmp1 - tmp6, 5) & DArRANGE_MASK];
        outptr[2] = range_limit[DArDESCALE(tmp2 + tmp5, 5) & DArRANGE_MASK];
        outptr[5] = range_limit[DArDESCALE(tmp2 - tmp5, 5) & DArRANGE_MASK];
        outptr[4] = range_limit[DArDESCALE(tmp3 + tmp4, 5) & DArRANGE_MASK];
        outptr[3] = range_limit[DArDESCALE(tmp3 - tmp4, 5) & DArRANGE_MASK];

        wsptr += DArCDJ_DCT_SIZE_1D; /* advance pointer to next row */
    }
}

/*--------------------------- FFmpeg glue ---------------------------*/

/*
 * The YCbCr working buffer is laid out as three MCU-aligned planes.  Luma has
 * a stride of 8 * MCUx * sampX and occupies sampX * sampY * unit bytes, where
 * unit = 64 * MCUx * MCUy is one chroma plane; Cb follows at sampX * sampY *
 * unit and Cr at twice that.  (Cr's base leaves a hole for the subsampled
 * ratios; that matches the reference layout and must be preserved.)
 */
static void odh_plane_layout(const SArCDJ_OdhMaster *ctrl, int *luma_stride,
                             int *chroma_stride, size_t *cb_off, size_t *cr_off,
                             size_t *total)
{
    const size_t unit = (size_t)DArCDJ_DCT_SIZE_2D *
                        ctrl->mMCUinImage[DArCDJ_AXIS_X] *
                        ctrl->mMCUinImage[DArCDJ_AXIS_Y];
    const size_t samp = (size_t)ctrl->mSmpRate[DArCDJ_AXIS_X] *
                        ctrl->mSmpRate[DArCDJ_AXIS_Y];

    *luma_stride   = DArCDJ_DCT_SIZE_1D * ctrl->mMCUinImage[DArCDJ_AXIS_X] *
                     ctrl->mSmpRate[DArCDJ_AXIS_X];
    *chroma_stride = DArCDJ_DCT_SIZE_1D * ctrl->mMCUinImage[DArCDJ_AXIS_X];
    *cb_off = samp * unit;
    *cr_off = 2 * samp * unit;
    *total  = 3 * samp * unit;
}

static void odh_samp_from_pix_fmt(enum AVPixelFormat fmt, int *samp_x, int *samp_y)
{
    switch (fmt) {
    case AV_PIX_FMT_YUV422P: *samp_x = 2; *samp_y = 1; break;
    case AV_PIX_FMT_YUV440P: *samp_x = 1; *samp_y = 2; break;
    case AV_PIX_FMT_YUV420P: *samp_x = 2; *samp_y = 2; break;
    default:                 *samp_x = 1; *samp_y = 1; break;
    }
}

static enum AVPixelFormat odh_pix_fmt(int samp_x, int samp_y)
{
    if (samp_x == 1 && samp_y == 1)
        return AV_PIX_FMT_YUV444P;
    if (samp_x == 2 && samp_y == 1)
        return AV_PIX_FMT_YUV422P;
    if (samp_x == 1 && samp_y == 2)
        return AV_PIX_FMT_YUV440P;
    return AV_PIX_FMT_YUV420P;
}

static int odh_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                            int *got_frame, AVPacket *avpkt)
{
    SArCDJ_OdhMaster ctrl;
    u8 *work = NULL;
    int luma_stride, chroma_stride, ret, i;
    size_t cb_off, cr_off, total;
    int width, height, cw, ch;
    u32 result;

    if (avpkt->size < DArCDJ_HEADER_SIZE)
        return AVERROR_INVALIDDATA;

    if (memcmp(avpkt->data, "AJPG", 4)) {
        av_log(avctx, AV_LOG_ERROR, "not an ODH image (bad AJPG magic)\n");
        return AVERROR_INVALIDDATA;
    }

    memset(&ctrl, 0, sizeof(ctrl));

    /*
     * Sizing needs the header parsed, but the parse also arms the whole
     * decode, so run it against a NULL work buffer first and only then
     * allocate.  cdj_d_initializeDecompressOdh does not touch the buffer.
     */
    result = cdj_d_initializeDecompressOdh(&ctrl, NULL, avpkt->data,
                                           avpkt->size);
    if (result) {
        av_log(avctx, AV_LOG_ERROR, "invalid ODH header (%08x)\n", result);
        return AVERROR_INVALIDDATA;
    }

    width  = ctrl.mImageSize[DArCDJ_AXIS_X];
    height = ctrl.mImageSize[DArCDJ_AXIS_Y];

    ret = ff_set_dimensions(avctx, width, height);
    if (ret < 0)
        return ret;

    avctx->pix_fmt     = odh_pix_fmt(ctrl.mSmpRate[DArCDJ_AXIS_X],
                                     ctrl.mSmpRate[DArCDJ_AXIS_Y]);
    avctx->color_range = AVCOL_RANGE_JPEG;

    odh_plane_layout(&ctrl, &luma_stride, &chroma_stride, &cb_off, &cr_off,
                     &total);

    work = av_mallocz(total);
    if (!work)
        return AVERROR(ENOMEM);
    ctrl.mImgYCbCrBufferPtr = work;

    result = cdj_d_decompressLoop(&ctrl);
    if (result) {
        av_log(avctx, AV_LOG_ERROR, "ODH decode failed (%08x)\n", result);
        av_free(work);
        return AVERROR_INVALIDDATA;
    }

    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0) {
        av_free(work);
        return ret;
    }

    cw = AV_CEIL_RSHIFT(width,  ctrl.mSmpRate[DArCDJ_AXIS_X] - 1);
    ch = AV_CEIL_RSHIFT(height, ctrl.mSmpRate[DArCDJ_AXIS_Y] - 1);

    for (i = 0; i < height; i++)
        memcpy(frame->data[0] + i * frame->linesize[0],
               work + (size_t)i * luma_stride, width);
    for (i = 0; i < ch; i++) {
        memcpy(frame->data[1] + i * frame->linesize[1],
               work + cb_off + (size_t)i * chroma_stride, cw);
        memcpy(frame->data[2] + i * frame->linesize[2],
               work + cr_off + (size_t)i * chroma_stride, cw);
    }

    av_free(work);

    frame->pict_type = AV_PICTURE_TYPE_I;
    frame->flags    |= AV_FRAME_FLAG_KEY;
    *got_frame = 1;

    return avpkt->size;
}

typedef struct ODHEncContext {
    const AVClass *class;
    int quality;
} ODHEncContext;

static av_cold int odh_encode_init(AVCodecContext *avctx)
{
    if (avctx->width  > DArCDJ_PIXEL_SIZE_MAX_X ||
        avctx->height > DArCDJ_PIXEL_SIZE_MAX_Y) {
        av_log(avctx, AV_LOG_ERROR,
               "ODH images are limited to %dx%d\n",
               DArCDJ_PIXEL_SIZE_MAX_X, DArCDJ_PIXEL_SIZE_MAX_Y);
        return AVERROR(EINVAL);
    }
    /* The reference encoder rejects odd dimensions outright. */
    if (avctx->width & 1 || avctx->height & 1) {
        av_log(avctx, AV_LOG_ERROR, "ODH requires even dimensions\n");
        return AVERROR(EINVAL);
    }
    return 0;
}

static int odh_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                            const AVFrame *frame, int *got_packet)
{
    ODHEncContext *s = avctx->priv_data;
    SArCDJ_OdhMaster ctrl;
    u8 *work = NULL;
    int luma_stride, chroma_stride, ret, i;
    size_t cb_off, cr_off, total;
    int quality = s->quality;
    u32 result, size_limit, written;
    int samp_x, samp_y, cw, ch;

    odh_samp_from_pix_fmt(avctx->pix_fmt, &samp_x, &samp_y);
    cw = AV_CEIL_RSHIFT(avctx->width,  samp_x - 1);
    ch = AV_CEIL_RSHIFT(avctx->height, samp_y - 1);

    /*
     * The reference sizes its output at 2 bytes per pixel (the RGB565 source
     * size) and retries at a lower quality when the entropy coder overruns.
     * Give it that budget plus the header, rounded to the 4-byte multiple
     * cdj_c_flashBuffer pads to.
     */
    size_limit = (u32)avctx->width * avctx->height * 2 + DArCDJ_HEADER_SIZE;
    size_limit = (size_limit + 3) & ~3u;

    ret = ff_alloc_packet(avctx, pkt, size_limit);
    if (ret < 0)
        return ret;

    for (;;) {
        u16 imgSize[2] = { (u16)avctx->width, (u16)avctx->height };

        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.mSmpRate[DArCDJ_AXIS_X] = samp_x;
        ctrl.mSmpRate[DArCDJ_AXIS_Y] = samp_y;

        result = cdj_c_initializeCompressOdh(&ctrl, imgSize, (u8)quality,
                                             NULL, pkt->data, size_limit);
        if (result) {
            av_log(avctx, AV_LOG_ERROR, "ODH encoder init failed (%08x)\n",
                   result);
            av_freep(&work);
            return AVERROR(EINVAL);
        }

        if (!work) {
            odh_plane_layout(&ctrl, &luma_stride, &chroma_stride, &cb_off,
                             &cr_off, &total);
            work = av_mallocz(total);
            if (!work)
                return AVERROR(ENOMEM);

            for (i = 0; i < avctx->height; i++)
                memcpy(work + (size_t)i * luma_stride,
                       frame->data[0] + (size_t)i * frame->linesize[0],
                       avctx->width);
            for (i = 0; i < ch; i++) {
                memcpy(work + cb_off + (size_t)i * chroma_stride,
                       frame->data[1] + (size_t)i * frame->linesize[1], cw);
                memcpy(work + cr_off + (size_t)i * chroma_stride,
                       frame->data[2] + (size_t)i * frame->linesize[2], cw);
            }
        }
        ctrl.mImgYCbCrBufferPtr = work;

        result = cdj_c_compressLoop(&ctrl);
        if (result != DArCDJRESULT_ERR_CODE_SIZE)
            break;

        /* Overran the buffer: step the quality down and try again. */
        quality -= 5;
        if (quality <= 0) {
            av_log(avctx, AV_LOG_ERROR,
                   "ODH image does not fit in %u bytes at any quality\n",
                   size_limit);
            av_freep(&work);
            return AVERROR(EINVAL);
        }
        av_log(avctx, AV_LOG_VERBOSE,
               "ODH output overran the size limit, retrying at quality %d\n",
               quality);
    }

    av_freep(&work);

    if (result & DArCDJRESULT_ERR_BIT) {
        av_log(avctx, AV_LOG_ERROR, "ODH encode failed (%08x)\n", result);
        return AVERROR(EINVAL);
    }

    /*
     * cdj_c_compressLoop already flushed the bit buffer and wrote the header
     * on the way out; its success return is the byte count, not a status.
     */
    written = result;

    pkt->size = written;
    *got_packet = 1;

    return 0;
}

#define OFFSET(x) offsetof(ODHEncContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM

static const AVOption odh_options[] = {
    { "quality", "Compression quality, JPEG-style", OFFSET(quality),
      AV_OPT_TYPE_INT, { .i64 = 75 }, 1, 100, VE },
    { NULL },
};

static const AVClass odh_encoder_class = {
    .class_name = "odh encoder",
    .item_name  = av_default_item_name,
    .option     = odh_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_odh_decoder = {
    .p.name         = "odh",
    CODEC_LONG_NAME("ODH (AJPG) image"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_ODH,
    .p.capabilities = AV_CODEC_CAP_DR1,
    FF_CODEC_DECODE_CB(odh_decode_frame),
};

const FFCodec ff_odh_encoder = {
    .p.name         = "odh",
    CODEC_LONG_NAME("ODH (AJPG) image"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_ODH,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_FRAME_THREADS,
    .p.priv_class   = &odh_encoder_class,
    .priv_data_size = sizeof(ODHEncContext),
    .init           = odh_encode_init,
    FF_CODEC_ENCODE_CB(odh_encode_frame),
    CODEC_PIXFMTS(AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV422P,
                  AV_PIX_FMT_YUV440P, AV_PIX_FMT_YUV420P),
    .color_ranges   = AVCOL_RANGE_JPEG,
};
