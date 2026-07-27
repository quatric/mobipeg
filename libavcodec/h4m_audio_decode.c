#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <stddef.h>

// ABI compatibility with HVQM4 1.3 is still kinda broken,
// but native decoding is bit-perfect *shrug*
// 1.5 increased the size of some fields from 16 to 32 bits
// and added per-plane motion vector modes

// define this to be ABI compatible with most HVQM4 1.3 binaries
//#define VERSION_1_3

// define this to be ABI compatible with Frogger Beyond
//#define FROGGER

// some things are per plane (luma, 2x chroma)
#define PLANE_COUNT 3
// some things are only split between luma and chroma
#define LUMA_CHROMA 2
#define LUMA_IDX 0
#define CHROMA_IDX 1

#ifndef NATIVE
#pragma pack(1)

static void bla()
{
    fputs("called an uninitialized function pointer\n", stderr);
    exit(EXIT_FAILURE);
}

#define SYMBOLT(x, T) T (*p##x)() = (void*)bla;
#include "symbols.inc"
#undef SYMBOLT
#endif

/* .h4m (HVQM4 1.3/1.5) audio decoder 0.4 by flacs/hcs */

//#define VERBOSE_PRINT

/* big endian */

static uint8_t get8(FILE *infile)
{
    uint8_t buf[1];
    if (1 != fread(buf, 1, 1, infile))
    {
        fprintf(stderr, "read error at 0x%lx\n", (unsigned long)ftell(infile));
        exit(EXIT_FAILURE);
    }

    return buf[0];
}

static uint16_t read16(void const * buf)
{
    uint32_t v = 0;
    for (int i = 0; i < 2; i++)
    {
        v <<= 8;
        v |= ((uint8_t const *)buf)[i];
    }
    return v;
}

static uint16_t get16(FILE * infile)
{
    uint8_t buf[2];
    if (2 != fread(buf, 1, 2, infile))
    {
        fprintf(stderr, "read error at 0x%lx\n", (unsigned long)ftell(infile));
        exit(EXIT_FAILURE);
    }
    return read16(buf);
}

static uint32_t read32(void const * buf)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
    {
        v <<= 8;
        v |= ((uint8_t const*)buf)[i];
    }
    return v;
}

static uint32_t get32(FILE * infile)
{
    uint8_t buf[4];
    if (4 != fread(buf, 1, 4, infile))
    {
        fprintf(stderr, "read error at 0x%lx\n", (unsigned long)ftell(infile));
        exit(EXIT_FAILURE);
    }
    return read32(buf);
}

static void expect32(uint32_t expected, FILE * infile)
{
    uint32_t v = get32(infile);
    if (v != expected)
    {
        fprintf(stderr, "expected 0x%08"PRIx32" at 0x%lx, got 0x%08"PRIx32"\n",
            expected, ftell(infile)-4, v);
        exit(EXIT_FAILURE);
    }
}

static void expect32_imm(uint32_t expected, uint32_t actual, unsigned long offset)
{
    if (expected != actual)
    {
        fprintf(stderr, "expected 0x%08lx to be 0x%08"PRIx32", got 0x%08"PRIx32"\n",
            offset, expected, actual);
        exit(EXIT_FAILURE);
    }
}

static void expect32_text(uint32_t expected, uint32_t actual, char const *name)
{
    if (expected != actual)
    {
        fprintf(stderr, "expected %s to be 0x%08"PRIx32", got 0x%08"PRIx32"\n",
            name, expected, actual);
        exit(EXIT_FAILURE);
    }
}

static void seek_past(uint32_t offset, FILE * infile)
{
    if (-1 == fseek(infile, offset, SEEK_CUR))
    {
        fprintf(stderr, "seek by 0x%x failed\n", offset);
        exit(EXIT_FAILURE);
    }
}

/* audio decode */

struct audio_state
{
    struct
    {
        int16_t hist;
        int8_t idx;
    } *ch;
};

const int32_t IMA_Steps[89] =
{
    7, 8, 9, 10, 11, 12, 13, 14,
    16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767

};

const int8_t IMA_IndexTable[16] = 

{
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8 
};

static int16_t clamp16(int32_t v)
{
    if (v > INT16_MAX) return INT16_MAX;
    else if (v < INT16_MIN) return INT16_MIN;
    return v;
}

__attribute__((unused))
static void decode_audio(struct audio_state *state, int first_aud, uint32_t sample_count, FILE *infile, FILE *outfile, int channels)
{
    int16_t * samples = samples = malloc(sample_count*sizeof(int16_t)*channels);
    uint32_t t = 0, i = 0;

    if (first_aud)
    {
        for (int c = channels - 1; c >= 0; c--)
        {
            state->ch[c].hist = get8(infile);
            state->ch[c].hist <<= 8;
            uint8_t b = get8(infile);
            state->ch[c].hist |= (b & 0x80);
            state->ch[c].idx = b & 0x7f;

            if (state->ch[c].idx > 88)
            {
                fprintf(stderr, "invalid step index (%d) at 0x%lx\n", state->ch[c].idx, ftell(infile));
                exit(EXIT_FAILURE);
            }
        }

        for (int c = 0; c < channels; c++)
        {
            samples[t++] = state->ch[c].hist;
        }
        i ++;
    }

    uint8_t b;
    int bitsleft = 0;
    for (; i < sample_count; i++)
    {
        for (int c = channels-1; c >= 0; c--)
        {
            if (bitsleft == 0)
            {
                b = get8(infile);
                bitsleft = 8;
            }

            int32_t ima_step = IMA_Steps[state->ch[c].idx];
            int32_t ima_delta = ima_step >> 3;
            if (b & 0x10) ima_delta += ima_step >> 2;
            if (b & 0x20) ima_delta += ima_step >> 1;
            if (b & 0x40) ima_delta += ima_step;

            if (b & 0x80) state->ch[c].hist = clamp16(state->ch[c].hist - ima_delta);
            else state->ch[c].hist = clamp16(state->ch[c].hist + ima_delta);
            state->ch[c].idx += IMA_IndexTable[(b&0xf0)>>4];
            if (state->ch[c].idx > 88) state->ch[c].idx = 88;
            if (state->ch[c].idx < 0) state->ch[c].idx = 0;

            b <<= 4;
            bitsleft -= 4;
        }

        for (int c = 0; c < channels; c++)
        {
            samples[t++] = state->ch[c].hist;
        }
    }

    if (channels)
    {
        if (sample_count != fwrite(samples, sizeof(int16_t)*channels, sample_count, outfile))
        {
            perror("fwrite");
            exit(EXIT_FAILURE);
        }
    }

    free(samples);
}

/* video stuff */

static int32_t divTable[0x10];
static int32_t mcdivTable[0x200];

static void init_global_constants(void)
{
    divTable[0] = 0;
    mcdivTable[0] = 0;
    for (int i = 1; i < 0x10; ++i)
        divTable[i] = 0x1000 / (i * 16) * 16;
    for (int i = 1; i < 0x200; ++i)
        mcdivTable[i] = 0x1000 / i;
}

static void HVQM4InitDecoder(void)
{
    init_global_constants();
}

// 4x4 block of single value
static void dcBlock(uint8_t *dst, uint32_t stride, uint8_t value)
{
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            dst[y * stride + x] = value;
}

static uint8_t saturate(int32_t x)
{
    return x < 0 ? 0 : x > 0xFF ? 0xFF : x;
}

static uint8_t sat_mean8(uint32_t u)
{
    return saturate((u + 4) / 8);
}

// 4x4 block
static void WeightImBlock(uint8_t *dst, uint32_t stride, uint8_t value, uint8_t top, uint8_t bottom, uint8_t left, uint8_t right)
{
    /*

    +---+---+---+
    |   | T |   |
    +---+---+---+
    | L | D | R |
    +---+---+---+
    |   | B |   |
    +---+---+---+

     */
    int32_t tmb = top - bottom;
    int32_t lmr = left - right;
    int32_t vph = tmb + lmr;
    int32_t vmh = tmb - lmr;

    int32_t v2 = value * 2;
    int32_t v8 = value * 8;

    int32_t tpl = (top    + left ) - v2;
    int32_t tpr = (top    + right) - v2;
    int32_t bpr = (bottom + right) - v2;
    int32_t bpl = (bottom + left ) - v2;

    int32_t tml = top    - left;
    int32_t tmr = top    - right;
    int32_t bmr = bottom - right;
    int32_t bml = bottom - left;

    // V:
    // 6  8  8 6
    // 8 10 10 8
    // 8 10 10 8
    // 6  8  8 6
    //
    // T:
    //  2  2  2  2
    //  0  0  0  0
    // -1 -1 -1 -1
    // -1 -1 -1 -1
    //
    // B/L/R: like T but rotated accordingly

    // (6*V + 2*T - B + 2*L -   R + 4) / 8
    // (8*V + 2*T - B       -   R + 4) / 8
    // (8*V + 2*T - B -   L       + 4) / 8
    // (6*V + 2*T - B -   L + 2*R + 4) / 8

    dst[0] = sat_mean8(v8 + vph + tpl);
    dst[1] = sat_mean8(v8 + vph + tml);
    dst[2] = sat_mean8(v8 + vmh + tmr);
    dst[3] = sat_mean8(v8 + vmh + tpr);

    dst += stride;

    // ( 8*V - B + 2*L -   R + 4) / 8
    // (10*V - B       -   R + 4) / 8
    // (10*V - B -   L       + 4) / 8
    // ( 8*V - B -   L + 2*R + 4) / 8

    dst[0] = sat_mean8(v8 + vph - tml);
    dst[1] = sat_mean8(v8 - bpr      );
    dst[2] = sat_mean8(v8 - bpl      );
    dst[3] = sat_mean8(v8 + vmh - tmr);

    dst += stride;

    // ( 8*V - T + 2*L - R + 4) / 8
    // (10*V - T       - R + 4) / 8
    // (10*V - T - L

    dst[0] = sat_mean8(v8 - vmh - bml);
    dst[1] = sat_mean8(v8 - tpr      );
    dst[2] = sat_mean8(v8 - tpl      );
    dst[3] = sat_mean8(v8 - vph - bmr);

    dst += stride;

    dst[0] = sat_mean8(v8 - vmh + bpl);
    dst[1] = sat_mean8(v8 - vmh + bml);
    dst[2] = sat_mean8(v8 - vph + bmr);
    dst[3] = sat_mean8(v8 - vph + bpr);
}

typedef struct
{
    uint32_t pos;
    int32_t root;
#if defined(VERSION_1_3) && !defined(FROGGER)
    uint16_t array[2][0x200];
#else
    uint32_t array[2][0x200];
#endif
} Tree;
#ifndef NATIVE
#if defined(VERSION_1_3) && !defined(FROGGER)
_Static_assert(sizeof(Tree) == 0x808, "sizeof(Tree) is incorrect");
#else
_Static_assert(sizeof(Tree) == 0x1008, "sizeof(Tree) is incorrect");
#endif
#endif

typedef struct
{
#if defined(VERSION_1_3) && !defined(FROGGER)
    void const *ptr;  // 0-3
    void const *start;  // 4-7 seems to be unused which is probably why it was removed in 1.5
    uint32_t size;     // 8-B
    uint8_t value;     // C
    uint8_t bit;       // D
    uint8_t pad[2]; // E-F
#else
    void const *ptr;   // 0-3
    uint32_t size;     // 4-7
    uint32_t value;    // 8-B
    int32_t bit;       // C-F
#endif
} BitBuffer;
#ifndef NATIVE
_Static_assert(sizeof(BitBuffer) == 0x10, "sizeof(BitBuffer) is incorrect");
#endif

typedef struct
{
    BitBuffer buf; // 0-F
    Tree *tree;    // 0x10-0x13
} BitBufferWithTree;
#ifndef NATIVE
_Static_assert(sizeof(BitBufferWithTree) == 0x14, "sizeof(BitBufferWithTree) is incorrect");
#endif

typedef struct
{
    uint8_t value;
    uint8_t type;
} BlockData;

typedef struct
{
    // size: 0x38 (56)
    BlockData *border; // 0-3 beginning of the plane including the border
    BlockData *payload; // 4-7 beginning of the non-border plane data
    uint16_t h_blocks; // 8-9
    uint16_t v_blocks; // A-B
    uint16_t h_blocks_safe; // C-D
    uint16_t v_blocks_safe; // E-F
    // offsets of PBs within one MCB
    // +---+---+
    // | 0 | 3 |
    // +---+---+
    // | 1 | 2 |
    // +---+---+
    uint16_t mcb_offset[4]; // 10-17
    // same for samples within one PB
    uint32_t pb_offset[4]; // 18-27
    uint16_t width_in_samples; // 28-29
    uint16_t height_in_samples; // 2A-2B
    uint32_t size_in_samples; // 2C-2F
    uint8_t width_shift; // 30
    uint8_t height_shift; // 31
    uint8_t pb_per_mcb_x; // 32  1..2
    uint8_t pb_per_mcb_y; // 33  1..2
    uint8_t blocks_per_mcb; // 34  1..4
    uint8_t padding[3]; // 35-37
} HVQPlaneDesc;
#ifndef NATIVE
_Static_assert(sizeof(HVQPlaneDesc) == 0x38, "sizeof(HVQPlaneDesc) is incorrect");
#endif

typedef struct
{
    // size: 0x6CD8 (plane data comes right after)
    HVQPlaneDesc planes[PLANE_COUNT]; // 0x00 - 0xA8
    Tree trees[6];
    BitBufferWithTree dc_values[PLANE_COUNT]; // DC values
    BitBufferWithTree dc_rle[PLANE_COUNT]; // DC run lengths
    BitBufferWithTree bufTree0[PLANE_COUNT];
    BitBufferWithTree basis_num[LUMA_CHROMA];
    BitBufferWithTree basis_num_run[LUMA_CHROMA];
    BitBuffer fixvl[PLANE_COUNT]; // uncompressed high-entropy data
    BitBufferWithTree mv_h; // horizontal motion vectors
    BitBufferWithTree mv_v; // vertical motion vectors
    BitBufferWithTree mcb_proc; // macroblock proc
    BitBufferWithTree mcb_type; // macroblock type
    uint16_t h_nest_size;
    uint16_t v_nest_size;
    uint8_t is_landscape; // FIXME: check what happens for square video
    uint8_t nest_data[70 * 38]; // 1.3: 0x3261 1.5: 0x6261
#if defined(VERSION_1_3) && !defined(FROGGER)
    uint8_t padding;
    uint16_t dc_max; // 0x3CC6
    uint16_t dc_min; // 0x3CC8
#else
    uint8_t padding[3];
    uint32_t dc_max; // 0x6CC8
    uint32_t dc_min; // 0x6CCC
#endif
    uint8_t unk_shift; // 1.3: 0x3CCA 1.5: 0x6CD0
    uint8_t dc_shift; // 0x6CD1
    // number of residual bits to read from mv_h/mv_v,
    // one setting for each of past and future
    uint8_t mc_residual_bits_h[2]; // 0x6CD2
    uint8_t mc_residual_bits_v[2]; // 0x6CD4
#if !defined(VERSION_1_3) || defined(FROGGER)
    uint8_t maybe_padding[2]; // 0x6CD6-0x6CD7
#endif
} VideoState;
#ifndef NATIVE
#if defined(VERSION_1_3) && !defined(FROGGER)
_Static_assert(sizeof(VideoState) == 0x3CD0, "sizeof(VideoState) is incorrect");
#else
_Static_assert(sizeof(VideoState) == 0x6CD8, "sizeof(VideoState) is incorrect");
#endif
#endif

typedef struct
{
    VideoState *state; // 0-3
    uint16_t width; // 4-5
    uint16_t height; // 6-7
    uint8_t h_samp; // 8
    uint8_t v_samp; // 9
} SeqObj;

typedef struct
{
    SeqObj seqobj;
    void *past;
    void *present;
    void *future;
} Player;

typedef struct
{
    uint16_t hres;
    uint16_t vres;
    uint8_t h_samp;
    uint8_t v_samp;
    uint8_t video_mode;
} VideoInfo;

// copy uncompressed 4x4 block
static void OrgBlock(VideoState *state, uint8_t *dst, uint32_t dst_stride, uint32_t plane_idx)
{
    BitBuffer *buf = &state->fixvl[plane_idx];
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            dst[y * dst_stride + x] = *(uint8_t*)buf->ptr++;
}

// can use FFmpeg's get_bits1()/get_bits(..., 8) for this
static int16_t getBit(BitBuffer *buf)
{
#if defined(VERSION_1_3) && !defined(FROGGER)
    int32_t bit = buf->bit;
    if (bit == 0)
    {
        buf->value = *(uint8_t const*)buf->ptr;
        buf->ptr += 1;
        bit = 0x80;
    }
    buf->bit = bit >> 1;
    return buf->value & bit ? 1 : 0;
#else
    int32_t bit = buf->bit;
    if (bit < 0)
    {
        buf->value = read32(buf->ptr);
        buf->ptr += 4;
        bit = 31;
    }
    buf->bit = bit - 1;
    return (buf->value >> bit) & 1;
#endif
}

static uint8_t getByte(BitBuffer *buf)
{
#if defined(VERSION_1_3) && !defined(FROGGER)
    uint8_t value = 0;
    for (int i = 7; i >= 0; --i)
        value |= getBit(buf) << i;
#else
    uint32_t value = buf->value;
    int32_t bit = buf->bit;
    if (bit < 7)
    {
        buf->value = read32(buf->ptr);
        buf->ptr += 4;
        value <<= 7 - bit;
        value |= buf->value >> (bit + 25);
        bit += 24;
    }
    else
    {
        value >>= bit - 7;
        bit -= 8;
    }
    buf->bit = bit;
    return value & 0xFF;
#endif
}

static uint32_t readTree_signed;
static uint32_t readTree_scale;

static int16_t _readTree(Tree *dst, BitBuffer *src)
{
    if (getBit(src) == 0)
    {
        // leaf node
        uint8_t byte = getByte(src);
        int16_t symbol = byte;
        if (readTree_signed && byte > 0x7F)
            symbol = (int8_t)byte;
        symbol <<= readTree_scale;
        dst->array[0][byte] = symbol;
        return byte;
    }
    else
    {
        // recurse
        uint32_t pos = dst->pos++;
        // read the 0 side of the tree
        dst->array[0][pos] = (uint32_t)_readTree(dst, src);
        // read the 1 side of the tree
        dst->array[1][pos] = (uint32_t)_readTree(dst, src);
        return (int16_t)pos;
    }
}

static void readTree(BitBufferWithTree *buf, uint32_t is_signed, uint32_t scale)
{
    readTree_signed = is_signed;
    readTree_scale = scale;
    Tree *tree = buf->tree;
    tree->pos = 0x100;
    if (buf->buf.size == 0)
        tree->root = 0;
    else
        tree->root = _readTree(tree, &buf->buf);
}

static uint32_t decodeHuff(BitBufferWithTree *buf)
{
    Tree *tree = buf->tree;
    int32_t pos = tree->root;
    while (pos >= 0x100)
        pos = tree->array[getBit(&buf->buf)][pos];
    return tree->array[0][pos];
}

// used for DC values
static int32_t decodeSOvfSym(BitBufferWithTree *buf, int32_t min, int32_t max)
{
    int32_t sum = 0;
    int32_t value;
    do
    {
        value = decodeHuff(buf);
        sum += value;
    } while (value <= min || value >= max);
    return sum;
}

// used for MCB proc/type, max is always 255
static int32_t decodeUOvfSym(BitBufferWithTree *buf, int32_t max)
{
    int32_t sum = 0;
    int32_t value;
    do
    {
        value = decodeHuff(buf);
        sum += value;
    } while (value >= max);
    return sum;
}

static uint32_t GetAotBasis(VideoState *state, uint8_t basis_out[4][4], int32_t *sum, uint8_t const *nest_data, uint32_t nest_stride, uint32_t plane_idx)
{
    BitBuffer *buf = &state->fixvl[plane_idx];

    // the nest size 70x38 is chosen to allow for
    // 6/5-bit coordinates (0..63 x 0..31) + largest sampling pattern (6x6) = 0..69 x 0..37
    // 0x003F: offset70 : 6
    // 0x07C0: offset38 : 5
    // 0x0800: stride70 : 1
    // 0x1000: stride38 : 1
    // 0x6000: offset   : 2
    // 0x8000: negated  : 1
    uint16_t bits = read16(buf->ptr);
    buf->ptr += 2;

    // compute the offset inside the nest
    uint32_t x_stride, y_stride;
    uint32_t offset70 = bits & 0x3F;
    uint32_t offset38 = (bits >> 6) & 0x1F;
    uint32_t stride70 = (bits >> 11) & 1;
    uint32_t stride38 = (bits >> 12) & 1;
    if (state->is_landscape)
    {
        nest_data += nest_stride * offset38 + offset70;
        x_stride =           1 << stride70;
        y_stride = nest_stride << stride38;
    }
    else
    {
        nest_data += nest_stride * offset70 + offset38;
        x_stride =           1 << stride38;
        y_stride = nest_stride << stride70;
    }

    // copy basis vector from the nest
    uint8_t min = nest_data[0];
    uint8_t max = nest_data[0];
    for (int y = 0; y < 4; ++y)
    {
        for (int x = 0; x < 4; ++x)
        {
            uint8_t nest_value = nest_data[y * y_stride + x * x_stride];
            basis_out[y][x] = nest_value;
            min = nest_value < min ? nest_value : min;
            max = nest_value > max ? nest_value : max;
        }
    }
    *sum += decodeHuff(&state->bufTree0[plane_idx]);
    int32_t inverse = divTable[max - min];
    if (bits & 0x8000)
        inverse = -inverse;
    int32_t offset = (bits >> 13) & 3;
    return (*sum + offset) * inverse;
}

static uint32_t GetMCAotBasis(VideoState *state, uint8_t basis_out[4][4], int32_t *sum, uint8_t const *nest_data, uint32_t nest_stride, uint32_t plane_idx)
{
    // the only difference to GetAotBasis() seems to be the ">> 4 & 0xF"
    BitBuffer *buf = &state->fixvl[plane_idx];
    uint16_t bits = read16(buf->ptr);
    buf->ptr += 2;
    uint32_t step, stride;
    uint32_t big = bits & 0x3F;
    uint32_t small = (bits >> 6) & 0x1F;
    if (state->is_landscape)
    {
        nest_data += nest_stride * small + big;
        step   =           1 << ((bits >> 11) & 1);
        stride = nest_stride << ((bits >> 12) & 1);
    }
    else
    {
        nest_data += nest_stride * big + small;
        step   =           1 << ((bits >> 12) & 1);
        stride = nest_stride << ((bits >> 11) & 1);
    }
    uint8_t min, max;
    min = max = (nest_data[0] >> 4) & 0xF; // !
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            uint8_t nest_value = (nest_data[i * stride + j * step] >> 4) & 0xF; // !
            basis_out[i][j] = nest_value;
            min = nest_value < min ? nest_value : min;
            max = nest_value > max ? nest_value : max;
        }
    }
    *sum += decodeHuff(&state->bufTree0[plane_idx]);
    int32_t inverse = divTable[max - min];
    if (bits & 0x8000)
        inverse = -inverse;
    int32_t foo = (bits >> 13) & 3;
    return (*sum + foo) * inverse;
}

static int32_t GetAotSum(VideoState *state, int32_t result[4][4], uint8_t num_bases, uint8_t const *nest_data, uint32_t nest_stride, uint32_t plane_idx)
{
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            result[y][x] = 0;
    uint8_t basis[4][4];
    int32_t temp = 0;
    for (int k = 0; k < num_bases; ++k)
    {
        uint32_t factor = GetAotBasis(state, basis, &temp, nest_data, nest_stride, plane_idx);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                result[y][x] += factor * basis[y][x];
    }
    int32_t sum = 0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            sum += result[y][x];
    int32_t mean = sum >> 4;
    return mean;
}

static int32_t GetMCAotSum(VideoState *state, int32_t result[4][4], uint8_t num_bases, uint8_t const *nest_data, uint32_t nest_stride, uint32_t plane_idx)
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            result[i][j] = 0;
    uint8_t byte_result[4][4];
    int32_t temp = 0;
    for (int k = 0; k < num_bases; ++k)
    {
        uint32_t factor = GetMCAotBasis(state, byte_result, &temp, nest_data, nest_stride, plane_idx);
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                result[i][j] += factor * byte_result[i][j];
    }
    int32_t sum = 0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            sum += result[i][j];
    int32_t mean = sum >> 4;
    return mean;
}

static void HVQM4InitSeqObj(SeqObj *seqobj, VideoInfo *videoinfo)
{
    seqobj->width = videoinfo->hres;
    seqobj->height = videoinfo->vres;
    // TODO: find better names for these, uv_h_step/uv_v_step?
    seqobj->h_samp = videoinfo->h_samp;
    seqobj->v_samp = videoinfo->v_samp;
}

static uint32_t HVQM4BuffSize(SeqObj *seqobj)
{
    uint32_t h_blocks = seqobj->width / 4;
    uint32_t v_blocks = seqobj->height / 4;
    uint32_t y_blocks = (h_blocks + 2) * (v_blocks + 2);

    uint32_t uv_h_blocks = seqobj->h_samp == 2 ? h_blocks / 2 : h_blocks;
    uint32_t uv_v_blocks = seqobj->v_samp == 2 ? v_blocks / 2 : v_blocks;
    uint32_t uv_blocks = (uv_h_blocks + 2) * (uv_v_blocks + 2);

    uint32_t total = (y_blocks + uv_blocks * 2) * sizeof(uint16_t);
    return sizeof(VideoState) + total;
}

// h_samp/v_samp: pixels per sample
static void setHVQPlaneDesc(SeqObj *seqobj, uint8_t plane_idx, uint8_t h_samp, uint8_t v_samp)
{
    HVQPlaneDesc *plane = &seqobj->state->planes[plane_idx];
    plane->width_shift = h_samp == 2 ? 1 : 0;
    plane->width_in_samples = seqobj->width >> plane->width_shift;
    plane->height_shift = v_samp == 2 ? 1 : 0;
    plane->height_in_samples = seqobj->height >> plane->height_shift;
    plane->size_in_samples = plane->width_in_samples * plane->height_in_samples;
    // pixels per 2x2 block
    plane->pb_per_mcb_x = 2 >> plane->width_shift;  // 1..2
    plane->pb_per_mcb_y = 2 >> plane->height_shift; // 1..2
    plane->blocks_per_mcb = plane->pb_per_mcb_x * plane->pb_per_mcb_y; // 1..4
    // number of 4x4 blocks
    plane->h_blocks = seqobj->width / (h_samp * 4);
    plane->v_blocks = seqobj->height / (v_samp * 4);
    // number of 4x4 blocks + border
    plane->h_blocks_safe = plane->h_blocks + 2;
    plane->v_blocks_safe = plane->v_blocks + 2;
    // offset of blocks in MCB
    plane->mcb_offset[0] = 0;
    plane->mcb_offset[1] = plane->h_blocks_safe;
    plane->mcb_offset[2] = plane->h_blocks_safe + 1;
    plane->mcb_offset[3] = 1;
    plane->pb_offset[0] = 0;
    plane->pb_offset[1] = plane->width_in_samples << 2;
    plane->pb_offset[2] = (plane->width_in_samples << 2) + 4;
    plane->pb_offset[3] = 4;
}

// HACK: assumes 4:2:0
__attribute__((unused))
static void dumpYUV(Player *player, char const *path)
{
    FILE *f = fopen(path, "wb+");
    uint32_t w = player->seqobj.width, h = player->seqobj.height;
    fprintf(f, "P5\n%u %u\n255\n", w, h * 2);
    uint8_t const *p = player->present;
    for (int plane = 0; plane < 2; ++plane)
    {
        for (uint32_t i = 0; i < h; ++i)
        {
            for (uint32_t j = 0; j < w; ++j)
            {
                if (plane == 0 || j < w / 2)
                    fputc(*p++, f);
                else
                    fputc(0, f);
            }
        }
    }
    fclose(f);
}

// HACK: assumes 4:2:0, assumes JPEG color space
static uint8_t clamp255(float f)
{
    return f < 0 ? 0 : f > 255 ? 255 : (uint8_t)f;
}
static void dumpRGB(Player *player, char const *path)
{
    FILE *f = fopen(path, "wb+");
    uint32_t w = player->seqobj.width, h = player->seqobj.height;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    uint8_t const *yp = player->present;
    uint8_t const *up = yp + w*h;
    uint8_t const *vp = up + w*h/4;
    uint8_t *rgb = malloc(w * h * 3);
    uint8_t *ptr = rgb;
    for (uint32_t i = 0; i < h; ++i)
    {
        for (uint32_t j = 0; j < w; ++j)
        {
            float y = yp[i   * w   + j];
            float u = up[i/2 * w/2 + j/2];
            float v = vp[i/2 * w/2 + j/2];
            *ptr++ = clamp255(y + 1.402f*(v - 128.f));
            *ptr++ = clamp255(y - 0.34414f*(u - 128.f) - 0.71414f*(v - 128.f));
            *ptr++ = clamp255(y + 1.772f*(u - 128.f));
        }
    }
    fwrite(rgb, w*h*3, 1, f);
    fclose(f);
    free(rgb);
}

__attribute__((unused))
static void dumpPlanes(VideoState *state, char const *prefix)
{
    for (int plane_idx = 0; plane_idx < PLANE_COUNT; ++plane_idx)
    {
        char path[128];
        snprintf(path, 128, "%s_%c.ppm", prefix, "yuv"[plane_idx]);
        FILE *f = fopen(path, "wb+");
        HVQPlaneDesc *plane = &state->planes[plane_idx];
        fprintf(f, "P5\n%u %u\n255\n", plane->h_blocks_safe, plane->v_blocks_safe);
        uint8_t const *p = (uint8_t const*)plane->border;
        for (int i = 0; i < plane->v_blocks_safe; ++i)
        {
            for (int j = 0; j < plane->h_blocks_safe; ++j)
            {
                fputc(*p++, f);
                ++p;
            }
        }
        fclose(f);
    }
}

static void set_border(BlockData *dst)
{
    dst->value = 0x7F;
    dst->type = 0xFF;
}

static void HVQM4SetBuffer(SeqObj *seqobj, void *workbuff)
{
    VideoState *state = workbuff;
    seqobj->state = state;
    setHVQPlaneDesc(seqobj, 0, 1, 1);
    setHVQPlaneDesc(seqobj, 1, seqobj->h_samp, seqobj->v_samp);
    setHVQPlaneDesc(seqobj, 2, seqobj->h_samp, seqobj->v_samp);

    state->is_landscape = seqobj->width >= seqobj->height;
    if (state->is_landscape)
    {
        state->h_nest_size = 70;
        state->v_nest_size = 38;
    }
    else
    {
        state->h_nest_size = 38;
        state->v_nest_size = 70;
    }

    state->basis_num[0].tree = &state->trees[3];
    state->basis_num[1].tree = &state->trees[3];

    state->basis_num_run[0].tree = &state->trees[1];
    state->basis_num_run[1].tree = &state->trees[1];

    state->dc_values[0].tree = &state->trees[0];
    state->dc_values[1].tree = &state->trees[0];
    state->dc_values[2].tree = &state->trees[0];

    state->dc_rle[0].tree = &state->trees[1]; // reuse!
    state->dc_rle[1].tree = &state->trees[1]; //
    state->dc_rle[2].tree = &state->trees[1]; //

    state->bufTree0[0].tree = &state->trees[2];
    state->bufTree0[1].tree = &state->trees[2];
    state->bufTree0[2].tree = &state->trees[2];

    state->mv_h.tree = &state->trees[4];
    state->mv_v.tree = &state->trees[4];

    state->mcb_proc.tree = &state->trees[5];
    state->mcb_type.tree = &state->trees[5];

    BlockData *plane_data = workbuff + sizeof(VideoState);
    for (int i = 0; i < PLANE_COUNT; ++i)
    {
        HVQPlaneDesc *plane = &state->planes[i];
        plane->border = plane_data;
        uint32_t stride = plane->h_blocks_safe;
        // skip top border (stride) and left border (1)
        plane->payload = plane_data + stride + 1;
        plane_data += plane->h_blocks_safe * plane->v_blocks_safe;

        // set horizontal borders
        BlockData *ptr = plane->border;
        for (uint32_t i = plane->h_blocks_safe; i; --i)
        {
            set_border(ptr);
            ++ptr;
        }

        ptr = plane_data;
        for (uint32_t i = plane->h_blocks_safe; i; --i)
        {
            --ptr;
            set_border(ptr);
        }

        // set vertical borders
        ptr = plane->border + stride;
        for (uint32_t i = plane->v_blocks_safe - 2; i; --i)
        {
            set_border(ptr);
            ptr += stride;
        }

        ptr = plane->border + stride * 2 - 1;
        for (uint32_t i = plane->v_blocks_safe - 2; i; --i)
        {
            set_border(ptr);
            ptr += stride;
        }
    }
}

static uint32_t getDeltaDC(VideoState *state, uint32_t plane_idx, uint32_t *rle)
{
    if (*rle == 0)
    {
        uint32_t delta = decodeSOvfSym(&state->dc_values[plane_idx], state->dc_min, state->dc_max);
        // successive zeroes are run-length encoded
        if (delta == 0)
            *rle = decodeHuff(&state->dc_rle[plane_idx]);
        return delta;
    }
    else
    {
        --*rle;
        return 0;
    }
}

// initialize a bit buffer
static void setCode(BitBuffer *dst, void const *src)
{
    dst->size = read32(src);
    dst->ptr = dst->size ? src + 4 : NULL;
#if defined(VERSION_1_3) && !defined(FROGGER)
    dst->start = dst->ptr;
    dst->bit = 0;
#else
    dst->bit = -1;
#endif
}

static void Ipic_BasisNumDec(VideoState *state)
{
    BlockData *luma_dst = state->planes[LUMA_IDX].payload;
    const uint32_t luma_h_blocks = state->planes[LUMA_IDX].h_blocks;
    const uint32_t luma_v_blocks = state->planes[LUMA_IDX].v_blocks;
    uint32_t rle = 0;
    for (uint32_t y = 0; y < luma_v_blocks; ++y)
    {
        for (uint32_t x = 0; x < luma_h_blocks; ++x)
        {
            if (rle)
            {
                luma_dst->type = 0;
                --rle;
            }
            else
            {
                int16_t num = decodeHuff(&state->basis_num[LUMA_IDX]) & 0xFFFF;
                if (num == 0)
                    rle = decodeHuff(&state->basis_num_run[LUMA_IDX]);
                luma_dst->type = num & 0xFF;
            }
            ++luma_dst;
        }
        // skip borders
        luma_dst += 2;
    }

    BlockData *u_dst = state->planes[1].payload;
    BlockData *v_dst = state->planes[2].payload;
    const uint32_t chroma_h_blocks = state->planes[CHROMA_IDX].h_blocks;
    const uint32_t chroma_v_blocks = state->planes[CHROMA_IDX].v_blocks;
    rle = 0;
    for (uint32_t y = 0; y < chroma_v_blocks; ++y)
    {
        for(uint32_t x = 0; x < chroma_h_blocks; ++x)
        {
            if (rle)
            {
                u_dst->type = 0;
                v_dst->type = 0;
                --rle;
            }
            else
            {
                int16_t num = decodeHuff(&state->basis_num[CHROMA_IDX]) & 0xFFFF;
                if (num == 0)
                    rle = decodeHuff(&state->basis_num_run[CHROMA_IDX]);
                u_dst->type = (num >> 0) & 0xF;
                v_dst->type = (num >> 4) & 0xF;
            }
            ++u_dst;
            ++v_dst;
        }
        u_dst += 2;
        v_dst += 2;
    }
}

static void IpicDcvDec(VideoState *state)
{
    for (int plane_idx = 0; plane_idx < PLANE_COUNT; ++plane_idx)
    {
        HVQPlaneDesc *plane = &state->planes[plane_idx];
        uint32_t rle = 0;
        const uint32_t v_blocks = plane->v_blocks;
        BlockData *curr = plane->payload;
        for (uint32_t y = 0; y < v_blocks; ++y)
        {
            // pointer to previous line
            BlockData const *prev = curr - plane->h_blocks_safe;
            // first prediction on a line is only the previous line's value
            uint8_t value = prev->value;
            for (uint32_t x = 0; x < plane->h_blocks; ++x)
            {
                value += getDeltaDC(state, plane_idx, &rle);
                curr->value = value;
                ++curr;
                ++prev;
                // next prediction on this line is the mean of left (current) and top values
                // +---+---+
                // |   | T |
                // +---+---+
                // | L | P |
                // +---+---+
                value = (value + prev->value + 1) / 2;
            }
            // skip right border of this line and left border of next line
            curr += 2;
        }
    }
}

static void MakeNest(VideoState *state, uint16_t nest_x, uint16_t nest_y)
{
    HVQPlaneDesc *y_plane = &state->planes[0];
    BlockData const *ptr = y_plane->payload + y_plane->h_blocks_safe * nest_y + nest_x;

    int32_t v_empty, h_empty, v_nest_blocks, h_nest_blocks, v_mirror, h_mirror;

    if (y_plane->h_blocks < state->h_nest_size)
    {
        // special case if the video is less than 280 pixels wide (assuming landscape mode)
        h_nest_blocks = y_plane->h_blocks;
        h_mirror = state->h_nest_size - y_plane->h_blocks;
        if (h_mirror > y_plane->h_blocks)
            h_mirror = y_plane->h_blocks;
        h_empty = state->h_nest_size - (h_nest_blocks + h_mirror);
    }
    else
    {
        h_nest_blocks = state->h_nest_size;
        h_empty = 0;
        h_mirror = 0;
    }

    if (y_plane->v_blocks < state->v_nest_size)
    {
        // special case if the video is less than 152 pixels high
        v_nest_blocks = y_plane->v_blocks;
        v_mirror = state->v_nest_size - y_plane->v_blocks;
        if (v_mirror > y_plane->v_blocks)
            v_mirror = y_plane->v_blocks;
        v_empty = state->v_nest_size - (v_nest_blocks + v_mirror);
    }
    else
    {
        v_nest_blocks = state->v_nest_size;
        v_empty = 0;
        v_mirror = 0;
    }

    uint8_t *nest = state->nest_data;
    for (int i = 0; i < v_nest_blocks; ++i)
    {
        BlockData const *p = ptr;
        for (int j = 0; j < h_nest_blocks; ++j)
        {
            *nest++ = (p->value >> 4) & 0xF;
            ++p;
        }
        // if the video is too small, mirror it
        for (int j = 0; j < h_mirror; ++j)
        {
            --p;
            *nest++ = (p->value >> 4) & 0xF;
        }
        // if it is still too small, null out the rest
        for (int j = 0; j < h_empty; ++j)
            *nest++ = 0;
        ptr += y_plane->h_blocks_safe;
    }

    // handle vertical mirroring
    uint8_t const *nest2 = nest - state->h_nest_size;
    for (int i = 0; i < v_mirror; ++i)
    {
        for (int j = 0; j < state->h_nest_size; ++j)
            *nest++ = nest2[j];
        nest2 -= state->h_nest_size;
    }

    // and vertical nulling
    for (int i = 0; i < v_empty; ++i)
        for (int j = 0; j < state->h_nest_size; ++j)
            *nest++ = 0;
}

// copy 4x4 samples without interpolation
static void _MotionComp_00(uint8_t *dst, uint32_t dst_stride, uint8_t const *src, uint32_t src_stride)
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            dst[i * dst_stride + j] = src[i * src_stride + j];
}

// offset vertically by half a sample
static void _MotionComp_01(uint8_t *dst, uint32_t dst_stride, uint8_t const *src, uint32_t src_stride)
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            dst[i * dst_stride + j] = (
                    src[(i + 0) * src_stride + j] +
                    src[(i + 1) * src_stride + j] + 1) / 2;
}

// offset horizontally by half a sample
static void _MotionComp_10(uint8_t *dst, uint32_t dst_stride, uint8_t const *src, uint32_t src_stride)
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            dst[i * dst_stride + j] = (
                    src[i * src_stride + j + 0] +
                    src[i * src_stride + j + 1] + 1) / 2;
}

// offset by half a sample in both directions
static void _MotionComp_11(uint8_t *dst, uint32_t dst_stride, uint8_t const *src, uint32_t src_stride)
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            dst[i * dst_stride + j] = (
                    src[(i + 0) * src_stride + j + 0] +
                    src[(i + 0) * src_stride + j + 1] +
                    src[(i + 1) * src_stride + j + 0] +
                    src[(i + 1) * src_stride + j + 1] + 2) >> 2;
}

// hpel = half-pixel offset
static void _MotionComp(void *dst, uint32_t dst_stride, void const *src, uint32_t src_stride, uint32_t hpel_dx, uint32_t hpel_dy)
{
    if (hpel_dy == 0)
        if (hpel_dx == 0)
            _MotionComp_00(dst, dst_stride, src, src_stride);
        else
            _MotionComp_10(dst, dst_stride, src, src_stride);
    else
        if (hpel_dx == 0)
            _MotionComp_01(dst, dst_stride, src, src_stride);
        else
            _MotionComp_11(dst, dst_stride, src, src_stride);
}

// payload format
//         7.....6.....5.....4.....3.....2.....1.....0.....
// byte 0: [                    pb_dc                     ]
// byte 1:       [ mcb type ][proc][         type         ]
// 
//   mcb type probably means
//     0: intra
//     1: inter - past
//     2: inter - future

typedef struct
{
    uint32_t rle; // init 0
    uint32_t pb_dc; // init 0x7F
    BlockData *payload_cur_blk; // 8
    BlockData *payload_cur_row; // 0xC
    void *present; // 0x10
    void *top; // 0x14
    void *target; // 0x18
    void *past; // 0x1C
    void *future; // 0x20
    uint16_t h_mcb_stride;
    uint16_t padding; // ?
    uint32_t v_mcb_stride;
    uint32_t pb_per_mcb_x;
    uint32_t stride;
} MCPlane;
#ifndef NATIVE
_Static_assert(sizeof(MCPlane) == 0x34, "sizeof(MCPlane) is wrong");
#endif

static void MotionComp(VideoState *state, MCPlane mcplanes[PLANE_COUNT], int32_t dx, int32_t dy)
{
    uint32_t hpel_dx = dx & 1;
    uint32_t hpel_dy = dy & 1;
    for (int i = 0; i < PLANE_COUNT; ++i)
    {
        MCPlane *mcplane = &mcplanes[i];
        HVQPlaneDesc *plane = &state->planes[i];
        int32_t plane_dx = dx >> plane->width_shift;
        int32_t plane_dy = dy >> plane->height_shift;
#ifndef VERSION_1_3
        if (state->padding[0])
        {
            hpel_dx = plane_dx & 1;
            hpel_dy = plane_dy & 1;
        }
#endif
        void *ptr = mcplane->target + (plane_dy >> 1) * plane->width_in_samples + (plane_dx >> 1);
        for (int j = 0; j < plane->blocks_per_mcb; ++j)
        {
            _MotionComp(mcplane->top + plane->pb_offset[j],
                        plane->width_in_samples,
                        ptr + plane->pb_offset[j],
                        plane->width_in_samples,
                        hpel_dx,
                        hpel_dy);
        }
    }
}

// aot = adaptive orthogonal transform
static void IntraAotBlock(VideoState *state, uint8_t *dst, uint32_t stride, uint8_t targetAverage, uint8_t block_type, uint32_t plane_idx)
{
    if (block_type == 6)
    {
        OrgBlock(state, dst, stride, plane_idx);
        return;
    }
    int32_t result[4][4];
    // block types 1..5 serve as number of bases to use, 9..15 are unused
    int32_t aotAverage = GetAotSum(state, result, block_type, state->nest_data, state->h_nest_size, plane_idx);
    int32_t delta = (targetAverage << state->unk_shift) - aotAverage;
    for (int y = 0; y < 4; ++y)
    {
        for (int x = 0; x < 4; ++x)
        {
            int32_t value = ((result[y][x] + delta) >> state->unk_shift);
            dst[y * stride + x] = saturate(value);
        }
    }
}

static void PrediAotBlock(VideoState *state, uint8_t *dst, uint8_t const *src, uint32_t stride, uint8_t block_type,
                          uint8_t *nest_data, uint32_t h_nest_size, uint32_t plane_idx, uint32_t hpel_dx, uint32_t hpel_dy)
{
    int32_t result[4][4];
    uint32_t aot_sum = GetMCAotSum(state, result, block_type - 1, nest_data, h_nest_size, plane_idx);

    uint8_t mdst[4][4];
    uint32_t const dst_stride = 4;
    _MotionComp(mdst, dst_stride, src, stride, hpel_dx, hpel_dy);
    int32_t mean = 8;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            mean += mdst[y][x];
    mean /= 16;
    int32_t diff[4][4];
    int32_t min, max;
    min = max = mdst[0][0] - mean;
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            int32_t value = diff[i][j] = mdst[i][j] - mean;
            min = value < min ? value : min;
            max = value > max ? value : max;
        }
    }
    uint32_t addend = (decodeSOvfSym(&state->dc_values[plane_idx], state->dc_min, state->dc_max) >> state->dc_shift << state->unk_shift) - aot_sum;
    uint32_t factor = (decodeSOvfSym(&state->dc_values[plane_idx], state->dc_min, state->dc_max) >> state->dc_shift);
    factor *= mcdivTable[max - min];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            result[i][j] += addend + diff[i][j] * factor;

    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            uint32_t value = (result[i][j] >> state->unk_shift) + mdst[i][j];
            dst[i * stride + j] = saturate(value);
        }
    }
}

typedef struct
{
    uint32_t plane_idx;
    BlockData const *line_prev;
    BlockData const *line_curr;
    BlockData const *line_next;
    BlockData next;
    BlockData curr;
    uint8_t value_prev;
} StackState;

static void IpicBlockDec(VideoState *state, uint8_t *dst, uint32_t stride, StackState *stack_state)
{
    if (stack_state->curr.type == 0)
    {
        uint8_t top    = stack_state->line_prev->type & 0x77 ? stack_state->curr.value : stack_state->line_prev->value;
        uint8_t bottom = stack_state->line_next->type & 0x77 ? stack_state->curr.value : stack_state->line_next->value;
        uint8_t right  = stack_state->next.type       & 0x77 ? stack_state->curr.value : stack_state->next.value;
        // the left value is tracked manually, the logic is equivalent with the other surrounding values
        uint8_t left   = stack_state->value_prev;
        WeightImBlock(dst, stride, stack_state->curr.value, top, bottom, left, right);
        stack_state->value_prev = stack_state->curr.value;
    }
    else if (stack_state->curr.type == 8)
    {
        dcBlock(dst, stride, stack_state->curr.value);
        stack_state->value_prev = stack_state->curr.value;
    }
    else
    {
        IntraAotBlock(state, dst, stride, stack_state->curr.value, stack_state->curr.type, stack_state->plane_idx);
        // don't use the current DC value to predict the next one
        stack_state->value_prev = stack_state->next.value;
    }
    // next block
    ++stack_state->line_prev;
    ++stack_state->line_next;
}

static void IpicLineDec(VideoState *state, uint8_t *dst, uint32_t stride, StackState *stack_state, uint16_t h_blocks)
{
    stack_state->next = stack_state->line_curr[0];
    stack_state->value_prev = stack_state->line_curr[0].value;

    while (--h_blocks > 0)
    {
        stack_state->curr = stack_state->next;
        ++stack_state->line_curr;
        stack_state->next = stack_state->line_curr[0];
        IpicBlockDec(state, dst, stride, stack_state);
        // next block on same line
        dst += 4;
    }

    stack_state->curr = stack_state->next;
    IpicBlockDec(state, dst, stride, stack_state);

    // skip current, right border on same line, and left border on next line
    stack_state->line_curr += 3;

    // these have already been advanced to the right border in IpicBlockDec
    stack_state->line_prev += 2;
    stack_state->line_next += 2;
}

static void IpicPlaneDec(VideoState *state, int plane_idx, uint8_t *dst)
{
    HVQPlaneDesc *plane = &state->planes[plane_idx];
    StackState stack_state;
    stack_state.plane_idx = plane_idx;
    stack_state.line_prev = plane->payload;
    stack_state.line_curr = plane->payload;
    stack_state.line_next = plane->payload + plane->h_blocks_safe;
    int16_t v_blocks = plane->v_blocks;
    // first line
    if (v_blocks > 0)
    {
        IpicLineDec(state, dst, plane->width_in_samples, &stack_state, plane->h_blocks);
        // blocks are 4x4 so advance dst by 4 lines
        dst += plane->width_in_samples * 4;
        --v_blocks;
    }
    // middle lines
    stack_state.line_prev = plane->payload;
    while (v_blocks > 1)
    {
        IpicLineDec(state, dst, plane->width_in_samples, &stack_state, plane->h_blocks);
        dst += plane->width_in_samples * 4;
        --v_blocks;
    }
    // last line
    if (v_blocks > 0)
    {
        stack_state.line_next = stack_state.line_curr;
        IpicLineDec(state, dst, plane->width_in_samples, &stack_state, plane->h_blocks);
    }
}

static void initMCHandler(VideoState *state, MCPlane mcplanes[PLANE_COUNT], void *present, void *past, void *future)
{
    for (int i = 0; i < PLANE_COUNT; ++i)
    {
        MCPlane *mcplane = &mcplanes[i];
        HVQPlaneDesc *plane = &state->planes[i];
        mcplane->rle = 0;
        mcplane->pb_dc = 0x7F;
        mcplane->present = present;
        mcplane->past    = past;
        mcplane->future  = future;
        mcplane->payload_cur_blk = plane->payload;
        mcplane->payload_cur_row = plane->payload;
        mcplane->h_mcb_stride = 8 >> plane->width_shift;
        mcplane->v_mcb_stride = plane->width_in_samples * (8 >> plane->height_shift);
        mcplane->pb_per_mcb_x = plane->pb_per_mcb_x;
        mcplane->stride = plane->h_blocks_safe * plane->pb_per_mcb_y;

        present += plane->size_in_samples;
        past    += plane->size_in_samples;
        future  += plane->size_in_samples;
    }
}

// run-length decoder
struct RLDecoder {
    uint32_t value;
    uint32_t count;
};

// stream of 0 or 1
static void initMCBproc(BitBufferWithTree *buftree, struct RLDecoder *proc)
{
    if (buftree->buf.ptr)
    {
        proc->value = getBit(&buftree->buf);
        proc->count = decodeUOvfSym(buftree, 0xFF);
    }
}

// stream of 0, 1 or 2
static void initMCBtype(BitBufferWithTree *buftree, struct RLDecoder *type)
{
    if (buftree->buf.ptr)
    {
        uint32_t value = getBit(&buftree->buf) << 1;
        type->value = value | getBit(&buftree->buf);
        type->count = decodeUOvfSym(buftree, 0xFF);
    }
}

// not confirmed: 
//
// the mcb type stream has one symbol per 8x8 block
// the mcb proc stream has one symbol wherever type != 0
//   type: 1001112010102211 |
//   proc: 0  0011 1 0 0110 |
// expressed as initial values and changes
//   type: 1- +  +++-+-- -  |
//   proc: 0    x    x  x x |
// and encoded as bitstreams (0 and 1 are literal bits, [n] are huffman-encoded values)
//          1   -   +   +   +   +   -   +   -   -   -
//   type: 01[1]1[2]0[3]0[1]0[1]0[1]1[1]0[1]1[1]1[2]1[2]
//   proc: 0[5][5][3][2][1]

static void setMCTop(MCPlane mcplanes[PLANE_COUNT])
{
    for (int i = 0; i < PLANE_COUNT; ++i)
        mcplanes[i].top = mcplanes[i].present;
}

static uint32_t mcbtypetrans[2][3] = {
    { 1, 2, 0 },
    { 2, 0, 1 },
};

static uint32_t getMCBtype(BitBufferWithTree *buftree, struct RLDecoder *type)
{
    if (type->count == 0)
    {
        // only three possible values, so when the value changes,
        // a single bit decides which other value to select
        // bit == 0 -> increment
        // bit == 1 -> decrement
        // then wrap to range 0..2
        uint32_t bit = getBit(&buftree->buf);
        type->value = mcbtypetrans[bit][type->value];
        type->count = decodeUOvfSym(buftree, 0xFF);
    }
    --type->count;
    return type->value;
}

static uint32_t getMCBproc(BitBufferWithTree *buftree, struct RLDecoder *proc)
{
    if (proc->count == 0)
    {
        proc->value ^= 1;
        proc->count = decodeUOvfSym(buftree, 0xFF);
    }
    --proc->count;
    return proc->value;
}

// advance one 8x8 block to the right
// (8x8 in seqobj size units, which are presumably pixels)
static void setMCNextBlk(MCPlane mcplanes[PLANE_COUNT])
{
    for (int i = 0; i < PLANE_COUNT; ++i)
    {
        mcplanes[i].top += mcplanes[i].h_mcb_stride;
        mcplanes[i].payload_cur_blk += mcplanes[i].pb_per_mcb_x;
    }
}

// move to the next row of 8x8 blocks
// (8x8 in seqobj size units, which are presumably pixels)
static void setMCDownBlk(MCPlane mcplanes[PLANE_COUNT])
{
    for (int i = 0; i < PLANE_COUNT; ++i)
    {
        MCPlane *mcplane = &mcplanes[i];
        mcplane->present += mcplane->v_mcb_stride;
        BlockData *first_block_on_next_row = mcplane->payload_cur_row + mcplane->stride;
        mcplane->payload_cur_blk = first_block_on_next_row;
        mcplane->payload_cur_row = first_block_on_next_row;
    }
}

static void decode_PB_dc(VideoState *state, MCPlane mcplanes[PLANE_COUNT])
{
    for (int i = 0; i < PLANE_COUNT; ++i)
    {
        HVQPlaneDesc *plane = &state->planes[i];
        MCPlane *mcplane = &mcplanes[i];
        for (int j = 0; j < plane->blocks_per_mcb; ++j)
        {
            mcplane->pb_dc += decodeSOvfSym(&state->dc_values[i], state->dc_min, state->dc_max);
            BlockData *payload = mcplane->payload_cur_blk;
            payload[plane->mcb_offset[j]].value = mcplane->pb_dc;
        }
    }
}

static void reset_PB_dc(MCPlane mcplanes[PLANE_COUNT])
{
    for (int i = 0; i < PLANE_COUNT; ++i)
        mcplanes[i].pb_dc = 0x7F;
}

static void decode_PB_cc(VideoState *state, MCPlane mcplanes[PLANE_COUNT], uint32_t proc, uint32_t type)
{
    uint32_t block_type = (type << 5) | (proc << 4);
    if (proc == 1)
    {
        for (int i = 0; i < PLANE_COUNT; ++i)
        {
            BlockData *payload = mcplanes[i].payload_cur_blk;
            HVQPlaneDesc *plane = &state->planes[i];
            for (int j = 0; j < plane->blocks_per_mcb; ++j)
                payload[plane->mcb_offset[j]].type = block_type;
        }
        return;
    }
    else
    {
        // luma
        HVQPlaneDesc *planeY = &state->planes[0];
        MCPlane *mcplaneY = &mcplanes[0];
        for (int i = 0; i < planeY->blocks_per_mcb; ++i)
        {
            BlockData *ptr = mcplaneY->payload_cur_blk;
            if (mcplaneY->rle)
            {
                ptr[planeY->mcb_offset[i]].type = block_type;
                --mcplaneY->rle;
            }
            else
            {
                int16_t huff = decodeHuff(&state->basis_num[LUMA_IDX]);
                if (huff)
                    ptr[planeY->mcb_offset[i]].type = block_type | huff;
                else
                {
                    ptr[planeY->mcb_offset[i]].type = block_type;
                    mcplaneY->rle = decodeHuff(&state->basis_num_run[0]);
                }
            }
        }
        // chroma
        HVQPlaneDesc *planeU = &state->planes[1];
        MCPlane *mcplaneU = &mcplanes[1];
        MCPlane *mcplaneV = &mcplanes[2];
        for (int i = 0; i < planeU->blocks_per_mcb; ++i)
        {
            BlockData *ptrU = mcplaneU->payload_cur_blk;
            BlockData *ptrV = mcplaneV->payload_cur_blk;
            if (mcplaneU->rle)
            {
                ptrU[planeU->mcb_offset[i]].type = block_type;
                ptrV[planeU->mcb_offset[i]].type = block_type;
                --mcplaneU->rle;
            }
            else
            {
                int16_t huff = decodeHuff(&state->basis_num[CHROMA_IDX]);
                if (huff)
                {
                    ptrU[planeU->mcb_offset[i]].type = block_type | ((huff >> 0) & 0xF);
                    ptrV[planeU->mcb_offset[i]].type = block_type | ((huff >> 4) & 0xF);
                }
                else
                {
                    ptrU[planeU->mcb_offset[i]].type = block_type;
                    ptrV[planeU->mcb_offset[i]].type = block_type;
                    mcplaneU->rle = decodeHuff(&state->basis_num_run[1]);
                }
            }
        }
    }
}

static void spread_PB_descMap(SeqObj *seqobj, MCPlane mcplanes[PLANE_COUNT])
{
    struct RLDecoder proc;
    struct RLDecoder type;
    VideoState *state = seqobj->state;
    initMCBproc(&state->mcb_proc, &proc);
    initMCBtype(&state->mcb_type, &type);
    for (int i = 0; i < seqobj->height; i += 8)
    {
        setMCTop(mcplanes);
        for (int j = 0; j < seqobj->width; j += 8)
        {
            getMCBtype(&state->mcb_type, &type);
            if (type.value == 0)
            {
                decode_PB_dc(state, mcplanes);
                decode_PB_cc(state, mcplanes, 0, type.value);
            }
            else
            {
                reset_PB_dc(mcplanes);
                decode_PB_cc(state, mcplanes, getMCBproc(&state->mcb_proc, &proc), type.value);
            }
            setMCNextBlk(mcplanes);
                // for all planes
                //     top             += h_mcb_stride
                //     payload_cur_blk += pb_per_mcb_x
        }
        setMCDownBlk(mcplanes);
            // for all planes
            //     present += v_mcb_stride
            //     payload_cur_row += stride;
            //     payload_cur_blk = payload_cur_row
    }
}

static void resetMCHandler(VideoState *state, MCPlane mcplanes[PLANE_COUNT], void *present)
{
    for (int i = 0; i < PLANE_COUNT; ++i)
    {
        mcplanes[i].present = present;
        mcplanes[i].payload_cur_blk = state->planes[i].payload;
        mcplanes[i].payload_cur_row = state->planes[i].payload;
        present += state->planes[i].size_in_samples;
    }
}

static void MCBlockDecDCNest(VideoState *state, MCPlane mcplanes[PLANE_COUNT])
{
    for (int plane_idx = 0; plane_idx < PLANE_COUNT; ++plane_idx)
    {
        BlockData const *ptr = mcplanes[plane_idx].payload_cur_blk;
        HVQPlaneDesc *plane = &state->planes[plane_idx];
        uint32_t stride = plane->width_in_samples;
        int32_t line = plane->h_blocks_safe;
        for (int j = 0; j < plane->blocks_per_mcb; ++j)
        {
            // dst is a 4x4 region
            uint8_t *dst = mcplanes[plane_idx].top + plane->pb_offset[j];
            int32_t block_idx = plane->mcb_offset[j];
            uint32_t value = ptr[block_idx].value;
            // block type:
            // 0: weighted
            // 6: literal block
            // 8: single value
            uint32_t type = ptr[block_idx].type & 0xF;
            // see also IpicBlockDec
            if (type == 0)
            {
                uint8_t top    = ptr[block_idx - line].type & 0x77 ? value : ptr[block_idx - line].value;
                uint8_t left   = ptr[block_idx -    1].type & 0x77 ? value : ptr[block_idx -    1].value;
                uint8_t right  = ptr[block_idx +    1].type & 0x77 ? value : ptr[block_idx +    1].value;
                uint8_t bottom = ptr[block_idx + line].type & 0x77 ? value : ptr[block_idx + line].value;
                WeightImBlock(dst, stride, value, top, bottom, left, right);
            }
            else if (type == 8)
            {
                dcBlock(dst, stride, value);
            }
            else
            {
                IntraAotBlock(state, dst, stride, value, type, plane_idx);
            }
        }
    }
}

// select which frame to use as an MC reference
static void setMCTarget(MCPlane mcplanes[PLANE_COUNT], uint32_t reference_frame)
{
    if (reference_frame == 0)
    {
        mcplanes[0].target = mcplanes[0].past;
        mcplanes[1].target = mcplanes[1].past;
        mcplanes[2].target = mcplanes[2].past;
    }
    else
    {
        mcplanes[0].target = mcplanes[0].future;
        mcplanes[1].target = mcplanes[1].future;
        mcplanes[2].target = mcplanes[2].future;
    }
}

static void getMVector(int32_t *result, BitBufferWithTree *buf, int32_t residual_bits)
{
    int32_t max_val_plus_1 = 1 << (residual_bits + 5);
    // quantized value
    int32_t value = decodeHuff(buf) << residual_bits;
    // residual bits
    for (int i = residual_bits - 1; i >= 0; --i)
        value += getBit(&buf->buf) << i;
    *result += value;
    // signed wrap to -max_val_plus_1 .. max_val_plus_1-1
    if (*result >= max_val_plus_1)
        *result -= max_val_plus_1 << 1;
    else if (*result < -max_val_plus_1)
        *result += max_val_plus_1 << 1;
}

static void MCBlockDecMCNest(VideoState *state, MCPlane mcplanes[PLANE_COUNT], int32_t x, int32_t y)
{
    void *nest_data;
    if (state->is_landscape)
        nest_data = mcplanes[0].target + x/2 + (y/2 - 16)*state->planes[0].width_in_samples - 32;
    else
        nest_data = mcplanes[0].target + x/2 + (y/2 - 32)*state->planes[0].width_in_samples - 16;
    uint32_t hpel_dx = x & 1;
    uint32_t hpel_dy = y & 1;
    for (int plane_idx = 0; plane_idx < PLANE_COUNT; ++plane_idx)
    {
        MCPlane *mcplane = &mcplanes[plane_idx];
        HVQPlaneDesc *plane = &state->planes[plane_idx];
        for (int i = 0; i < plane->blocks_per_mcb; ++i)
        {
            BlockData const *ptr = mcplane->payload_cur_blk;
            uint8_t block_type = ptr[plane->mcb_offset[i]].type & 0xF;
            // dst is a 4x4 region
            void *dst = mcplane->top + plane->pb_offset[i];
            uint32_t stride = plane->width_in_samples;
            if (block_type == 6)
            {
                OrgBlock(state, dst, stride, plane_idx);
            }
            else
            {
                int32_t plane_dx = x >> plane->width_shift;
                int32_t plane_dy = y >> plane->height_shift;
#ifndef VERSION_1_3
                if (state->padding[0])
                {
                    hpel_dx = plane_dx & 1;
                    hpel_dy = plane_dy & 1;
                }
#endif
                void const *src = mcplane->target + (plane_dy >> 1) * plane->width_in_samples + (plane_dx >> 1) + plane->pb_offset[i];
                if (block_type == 0)
                {
                    _MotionComp(dst, stride, src, stride, hpel_dx, hpel_dy);
                }
                else
                {
                    uint32_t strideY = state->planes[0].width_in_samples;
                    PrediAotBlock(state, dst, src, stride, block_type, nest_data, strideY, plane_idx, hpel_dx, hpel_dy);
                }
            }
        }
    }
}

static void BpicPlaneDec(SeqObj *seqobj, void *present, void *past, void *future)
{
    MCPlane mcplanes[PLANE_COUNT];
    VideoState *state = seqobj->state;
    initMCHandler(state, mcplanes, present, past, future);
    spread_PB_descMap(seqobj, mcplanes);
    resetMCHandler(state, mcplanes, present);
    int32_t mv_h, mv_v;
    int32_t reference_frame = -1;
    // MC blocks are 8x8 pixels
    for (int y = 0; y < seqobj->height; y += 8)
    {
        setMCTop(mcplanes);
        for (int x = 0; x < seqobj->width; x += 8)
        {
            uint8_t bits = mcplanes[0].payload_cur_blk->type;
            // 0: intra
            // 1: inter - past
            // 2: inter - future
            // see getMCBtype()
            int8_t new_reference_frame = (bits >> 5) & 3;
            if (new_reference_frame == 0)
            {
                // intra
                MCBlockDecDCNest(state, mcplanes);
            }
            else
            {
                // inter
                --new_reference_frame;
                // check if we need to update the reference frame pointers
                if (new_reference_frame != reference_frame)
                {
                    reference_frame = new_reference_frame;
                    setMCTarget(mcplanes, reference_frame);
                    mv_h = 0;
                    mv_v = 0;
                }
                getMVector(&mv_h, &state->mv_h, state->mc_residual_bits_h[reference_frame]);
                getMVector(&mv_v, &state->mv_v, state->mc_residual_bits_v[reference_frame]);

                // compute half-pixel position of reference macroblock
                uint32_t ref_x = x * 2 + mv_h;
                uint32_t ref_y = y * 2 + mv_v;

                // see getMCBproc()
                int mcb_proc = (bits >> 4) & 1;
                if (mcb_proc == 0)
                    MCBlockDecMCNest(state, mcplanes, ref_x, ref_y);
                else
                    MotionComp(state, mcplanes, ref_x, ref_y);
            }
            setMCNextBlk(mcplanes);
        }
        setMCDownBlk(mcplanes);
    }
}

static void HVQM4DecodeIpic(SeqObj *seqobj, uint8_t const *frame, void *present)
{
    VideoState *state = seqobj->state;
    uint8_t dc_shift = *frame++;
    state->unk_shift = *frame++;
    frame += 2; // unused, seems to be always zero
    uint16_t nest_x = read16(frame); frame += 2;
    uint16_t nest_y = read16(frame); frame += 2;
    uint8_t const *data = frame + 0x40;
    for (int i = 0; i < 2; ++i)
    {
        setCode(&state->basis_num[i].buf, data + read32(frame)); frame += 4;
        setCode(&state->basis_num_run[i].buf, data + read32(frame)); frame += 4;
    }
    for (int i = 0; i < PLANE_COUNT; ++i)
    {
        setCode(&state->dc_values[i].buf, data + read32(frame)); frame += 4;
        setCode(&state->bufTree0[i].buf,  data + read32(frame)); frame += 4;
        setCode(&state->fixvl[i],         data + read32(frame)); frame += 4;
    }
    for (int i = 0; i < PLANE_COUNT; ++i)
    {
        setCode(&state->dc_rle[i].buf, data + read32(frame)); frame += 4;
    }
    // multiple BitBufferWithTree instances share the same Tree,
    // the first BitBuffer of each group contains the Tree itself
    readTree(&state->basis_num[0], 0, 0);
    readTree(&state->basis_num_run[0], 0, 0);
    readTree(&state->dc_values[0], 1, dc_shift);
    readTree(&state->bufTree0[0], 0, 2);

    state->dc_max = +0x7F << dc_shift;
    state->dc_min = -0x80 << dc_shift;

    // 4x4 block types
    Ipic_BasisNumDec(state);
    // 4x4 block DC values
    IpicDcvDec(state);
    // 70x38 nest copied from upper 4 bits of DC values somewhere in the luma plane
    MakeNest(state, nest_x, nest_y);

    for (int i = 0; i < PLANE_COUNT; ++i)
    {
        IpicPlaneDec(state, i, present);
        present += state->planes[i].size_in_samples;
    }
}

static void HVQM4DecodeBpic(SeqObj *seqobj, uint8_t const *frame, void *present, void *past, void *future)
{
    VideoState *state = seqobj->state;
    state->dc_shift = frame[0];
    state->unk_shift = frame[1];
    state->mc_residual_bits_h[0] = frame[2];
    state->mc_residual_bits_v[0] = frame[3];
    state->mc_residual_bits_h[1] = frame[4];
    state->mc_residual_bits_v[1] = frame[5];
    // frame[6] and frame[7] are unused
    frame += 8;
    uint8_t const *data = frame + 0x44;
    for (int i = 0; i < 2; ++i)
    {
        setCode(&state->basis_num[i].buf,     data + read32(frame)); frame += 4;
        setCode(&state->basis_num_run[i].buf, data + read32(frame)); frame += 4;
    }
    for (int i = 0; i < PLANE_COUNT; ++i)
    {
        setCode(&state->dc_values[i].buf, data + read32(frame)); frame += 4;
        setCode(&state->bufTree0[i].buf,  data + read32(frame)); frame += 4;
        setCode(&state->fixvl[i],         data + read32(frame)); frame += 4;
    }
    setCode(&state->mv_h.buf, data + read32(frame)); frame += 4;
    setCode(&state->mv_v.buf, data + read32(frame)); frame += 4;
    setCode(&state->mcb_type.buf, data + read32(frame)); frame += 4;
    setCode(&state->mcb_proc.buf, data + read32(frame)); frame += 4;
    readTree(&state->basis_num[0], 0, 0);
    readTree(&state->basis_num_run[0], 0, 0);
    readTree(&state->dc_values[0], 1, state->dc_shift);
    readTree(&state->bufTree0[0], 0, 2);
    readTree(&state->mv_h, 1, 0);
    readTree(&state->mcb_type, 0, 0);

    state->dc_max = +0x7F << state->dc_shift;
    state->dc_min = -0x80 << state->dc_shift;

    BpicPlaneDec(seqobj, present, past, future);
}

static void HVQM4DecodePpic(SeqObj *seqobj, uint8_t const *frame, void *present, void *past)
{
    HVQM4DecodeBpic(seqobj, frame, present, past, present);
}

// I/P frames shift the past/future reference window
// B frames are never used as references
enum FrameType
{
    I_FRAME = 0x10,
    P_FRAME = 0x20,
    B_FRAME = 0x30,
};

#ifndef NATIVE
#define YOLO_INCLUDE
#include "yoloader.c"
#endif


static void decode_video(Player *player, FILE *infile, uint32_t gop_start, uint16_t frame_type, uint32_t frame_size)
{
    // getBit() and getByte() overread by up to 3 bytes
    uint32_t overread = 3;
    uint8_t *frame = malloc(frame_size + overread);
    fread(frame, frame_size, 1, infile);

    uint32_t disp_id = read32(frame);

    // swap past and future
    if (frame_type != B_FRAME)
    {
        void *tmp = player->past;
        player->past = player->future;
        player->future = tmp;
    }

    SeqObj *seqobj = &player->seqobj;
    switch (frame_type)
    {
#ifdef NATIVE
    case I_FRAME: putchar('I'); fflush(stdout);
                  HVQM4DecodeIpic(seqobj, frame + 4, player->present);                               break;
    case P_FRAME: putchar('P'); fflush(stdout);
                  HVQM4DecodePpic(seqobj, frame + 4, player->present, player->past);                 break;
    case B_FRAME: putchar('B'); fflush(stdout);
                  HVQM4DecodeBpic(seqobj, frame + 4, player->present, player->past, player->future); break;
#else
    case I_FRAME: putchar('I'); fflush(stdout);
                  pHVQM4DecodeIpic(seqobj, frame + 4, player->present);                               break;
    case P_FRAME: putchar('P'); fflush(stdout);
                  pHVQM4DecodePpic(seqobj, frame + 4, player->present, player->past);                 break;
    case B_FRAME: putchar('B'); fflush(stdout);
                  pHVQM4DecodeBpic(seqobj, frame + 4, player->present, player->past, player->future); break;
#endif
    default:
        fprintf(stderr, "unknown video frame type 0x%x\n", frame_type);
        exit(EXIT_FAILURE);
    }
    free(frame);

    //if (frame_type == I_FRAME)
    {
        char name[50];
        sprintf(name, "output/video_rgb_%u.ppm", gop_start + disp_id);
        //char type = "ipb"[(frame_type >> 4) - 1];
        //sprintf(name, "output/video_rgb_%u_%u_%c.ppm", gop, disp_id, type);
        //printf("writing frame to %s...\n", name);
        dumpRGB(player, name);
        //sprintf(name, "output/video_yuv_%u_%u_%c.ppm", gop, disp_id, type);
        //dumpYUV(player, name);
    }

    // swap present and future
    if (frame_type != B_FRAME)
    {
        void *tmp = player->present;
        player->present = player->future;
        player->future = tmp;
    }
}

/* stream structure */

const char HVQM4_13_magic[16] = "HVQM4 1.3";
const char HVQM4_15_magic[16] = "HVQM4 1.5";

typedef struct
{
    enum
    {
        HVQM4_13,
        HVQM4_15,
    } version;

    uint32_t header_size;   /* 0x10-0x13 */
    uint32_t body_size;     /* 0x14-0x17 */
    uint32_t blocks;        /* 0x18-0x1B */
    uint32_t video_frames;  /* 0x1C-0x1F */
    uint32_t audio_frames;  /* 0x20-0x23 */
    uint32_t usec_per_frame;/* 0x24-0x27 (33366, 33367, 40000) */
    uint32_t max_frame_sz;  /* 0x28-0x2B */
    uint32_t unk2C;         /* 0x2C-0x2F (0) */
    uint32_t audio_frame_sz;/* 0x30-0x33 */
    uint16_t hres;          /* 0x34-0x35 */
    uint16_t vres;          /* 0x36-0x37 */
    uint8_t  hsamp;         /* 0x38 */
    uint8_t  vsamp;         /* 0x39 */
    uint8_t  video_mode;    /* 0x3A (0 or 0x12) */
    uint8_t  unk3B;         /* 0x3B (0) */
    uint8_t  audio_channels;/* 0x3C */
    uint8_t  audio_bitdepth;/* 0x3D */
    uint8_t  audio_format;  /* 0x3E */
    uint8_t  audio_tracks;  /* 0x3F */
    uint32_t audio_srate;   /* 0x40-0x43 */
} HVQM4_header;

static void load_header(HVQM4_header *header, uint8_t *raw_header)
{
    /* check MAGIC */
    if (!memcmp(HVQM4_13_magic, &raw_header[0], 16))
    {
        header->version = HVQM4_13;
    }
    else if (!memcmp(HVQM4_15_magic, &raw_header[0], 16))
    {
        header->version = HVQM4_15;
    }
    else
    {
        fprintf(stderr, "does not appear to be a HVQM4 file\n");
        exit(EXIT_FAILURE);
    }

    header->header_size = read32(&raw_header[0x10]);
    header->body_size = read32(&raw_header[0x14]);
    header->blocks = read32(&raw_header[0x18]);
    header->video_frames = read32(&raw_header[0x1C]);
    header->audio_frames = read32(&raw_header[0x20]);
    header->usec_per_frame = read32(&raw_header[0x24]);
    header->max_frame_sz = read32(&raw_header[0x28]);
    header->unk2C = read32(&raw_header[0x2C]);
    header->audio_frame_sz = read32(&raw_header[0x30]);
    header->hres = read16(&raw_header[0x34]);
    header->vres = read16(&raw_header[0x36]);
    header->hsamp = raw_header[0x38];
    header->vsamp = raw_header[0x39];
    header->video_mode = raw_header[0x3A];
    header->unk3B = raw_header[0x3B];
    header->audio_channels = raw_header[0x3C];
    header->audio_bitdepth = raw_header[0x3D];
    header->audio_format = raw_header[0x3E];
    header->audio_tracks = raw_header[0x3F];
    header->audio_srate = read32(&raw_header[0x40]);

    expect32_text(0x44, header->header_size, "header size");
    /* no check for body size yet */
    if (header->blocks == 0)
    {
        fprintf(stderr, "zero blocks\n");
        exit(EXIT_FAILURE);
    }
#if 0
    if (header->audio_frames != 0)
    {
        if (header->audio_srate == 0 || header->audio_frame_sz == 0 || header->audio_channels == 0)
        {
            fprintf(stderr, "expected nonzero audio srate and frame size\n");
            exit(EXIT_FAILURE);
        }
    }
#endif
    /* no check for video frame count */
    /*
    if (header->unk24 != 0x8257 && header->unk24 != 0x8256)
    {
        expect32_imm(0x8257, header->unk24, 0x24);
    }
    */
    expect32_imm(0, header->unk2C, 0x2C);
    //expect32_text(0x650, header->audio_frame_sz, "audio frame size");
    /* no check for hres, vres */
    if (header->video_mode != 0 && header->video_mode != 0x12)
    {
        expect32_imm(0, header->video_mode, 0x3A);
    }
    expect32_imm(0, header->unk3B, 0x3B);
    //expect32_imm(0x02100000, header->unk3C, 0x3C);
    /* no check for srate, can be 0 */
}

static void display_header(HVQM4_header *header)
{
    switch (header->version)
    {
        case HVQM4_13:
            printf("HVQM4 1.3\n");
            break;
        case HVQM4_15:
            printf("HVQM4 1.5\n");
            break;
    }
    printf("Header size: 0x%"PRIx32"\n", header->header_size);
    printf("Body size:   0x%"PRIx32"\n", header->body_size);
    printf("Max frame:   0x%"PRIx32"\n", header->max_frame_sz);
    printf("Resolution:  %"PRIu32" x %"PRIu32"\n", header->hres, header->vres);
    printf("Chroma subsampling: %u x %u\n", header->hsamp, header->vsamp);
    printf("Video mode: %u\n", header->video_mode);
    printf("µs/frame:    %"PRIu32"\n", header->usec_per_frame);
    printf("%d GOPs\n", header->blocks);
    printf("%d Video frames\n", header->video_frames);
    printf("%d Audio frames\n", header->audio_frames);
    printf("Sample rate: %"PRIu32" Hz\n", header->audio_srate);
    printf("Audio frame size: 0x%"PRIx32"\n", header->audio_frame_sz);
    printf("Audio channels: %u\n", header->audio_channels);
    printf("\n");
}

static void put_32bitLE(uint8_t * buf, uint32_t v)
{
    for (unsigned int i = 0; i < 4; i++)
    {
        buf[i] = v & 0xFF;
        v >>= 8;
    }
}

static void put_16bitLE(uint8_t * buf, uint16_t v)
{
    for (unsigned int i = 0; i < 2; i++)
    {
        buf[i] = v & 0xFF;
        v >>= 8;
    }
}

/* make a header for 16-bit PCM .wav */
/* buffer must be 0x2c bytes */
static void make_wav_header(uint8_t * buf, int32_t sample_count, int32_t sample_rate, int channels) {
    size_t bytecount;

    bytecount = sample_count*channels*2;

    /* RIFF header */
    memcpy(buf+0, "RIFF", 4);
    /* size of RIFF */
    put_32bitLE(buf+4, (int32_t)(bytecount+0x2c-8));

    /* WAVE header */
    memcpy(buf+8, "WAVE", 4);

    /* WAVE fmt chunk */
    memcpy(buf+0xc, "fmt ", 4);
    /* size of WAVE fmt chunk */
    put_32bitLE(buf+0x10, 0x10);

    /* compression code 1=PCM */
    put_16bitLE(buf+0x14, 1);

    /* channel count */
    put_16bitLE(buf+0x16, channels);

    /* sample rate */
    put_32bitLE(buf+0x18, sample_rate);

    /* bytes per second */
    put_32bitLE(buf+0x1c, sample_rate*channels*2);

    /* block align */
    put_16bitLE(buf+0x20, (int16_t)(channels*2));

    /* significant bits per sample */
    put_16bitLE(buf+0x22, 2*8);

    /* PCM has no extra format bytes, so we don't even need to specify a count */

    /* WAVE data chunk */
    memcpy(buf+0x24, "data", 4);
    /* size of WAVE data chunk */
    put_32bitLE(buf+0x28, (int32_t)bytecount);
}

static void decv_init(Player *player)
{
    // HACK
    uint32_t res = player->seqobj.width * player->seqobj.height;
    uint32_t sample_size = player->seqobj.h_samp * player->seqobj.v_samp;
    uint32_t picsize = (res * (sample_size + 2)) / sample_size;

    player->past    = malloc(picsize);
    player->present = malloc(picsize);
    player->future  = malloc(picsize);
}

#ifndef HVQM4_FFMPEG
int main(int argc, char **argv)
{
    printf("h4m 'HVQM4 1.3/1.5' decoder 0.4 by flacs/hcs\n\n");
    if (argc != 3)
    {
        fprintf(stderr, "usage: %s file.h4m output.wav\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    /* open input */
    FILE *infile = fopen(argv[1], "rb");
    if (!infile)
    {
        fprintf(stderr, "failed opening %s\n", argv[1]);
        exit(EXIT_FAILURE);
    }

    /* read in header */
    uint8_t raw_header[0x44];
    if (0x44 != fread(&raw_header, 1, 0x44, infile))
    {
        fprintf(stderr, "failed reading header");
        exit(EXIT_FAILURE);
    }

    /* load up and check header */
    HVQM4_header header;
    load_header(&header, raw_header);
    display_header(&header);

    /* open output */
    FILE *outfile = fopen(argv[2], "wb");
    if (!outfile)
    {
        fprintf(stderr, "error opening %s\n", argv[2]);
        exit(EXIT_FAILURE);
    }

    /* fill in the space that we'll put the header in later */
    uint8_t riff_header[0x2c];
    memset(riff_header, 0, sizeof(riff_header));
    if (0x2c != fwrite(riff_header, 1, 0x2c, outfile))
    {
        fprintf(stderr, "error writing riff header\n");
        exit(EXIT_FAILURE);
    }

#ifndef NATIVE
    if (header.version == HVQM4_13)
        load_library("Frogger.elf");
    else
        load_library("RM_DLL.elf");
    // native version doesn't init clip LUT
    pHVQM4InitDecoder();
#endif

    HVQM4InitDecoder();

    Player player;
    HVQM4InitSeqObj(&player.seqobj, (VideoInfo*)&header.hres);
    VideoState *state = malloc(HVQM4BuffSize(&player.seqobj));
#ifndef VERSION_1_3
    // HACK to handle both 1.3 and 1.5
    state->padding[0] = header.version == HVQM4_15;
#endif
    HVQM4SetBuffer(&player.seqobj, state);
    decv_init(&player);

    /* parse blocks */
    uint32_t block_count = 0;
    uint32_t total_aud_frames = 0;
    uint32_t total_vid_frames = 0;
    uint32_t last_block_start = ftell(infile);
    uint32_t total_sample_count = 0;
    while (block_count < header.blocks)
    {
        const long block_start = ftell(infile);
        //expect32(ftell(infile) - last_block_start, infile);
        get32(infile);
        last_block_start = block_start;
        const uint32_t expected_block_size = get32(infile);
        const uint32_t expected_vid_frame_count = get32(infile);
        const uint32_t expected_aud_frame_count = get32(infile);
        expect32(0x01000000, infile);   /* EOS marker? */
        //get32(infile);   /* EOS marker? */
        const long data_start = ftell(infile);

        block_count ++;
#ifdef VERBOSE_PRINT
        printf("\n\nblock %u/%u starts at 0x%lx, length 0x%"PRIx32"\n", (int)block_count, header.blocks, block_start, expected_block_size);
#endif

        /* parse frames */
        struct audio_state audio_state;
        __attribute__((unused))
        int first_aud=1;
        uint32_t vid_frame_count = 0, aud_frame_count = 0;
        int block_sample_count =0;

        audio_state.ch = calloc(header.audio_channels, sizeof(*audio_state.ch));
        while (aud_frame_count < expected_aud_frame_count ||
               vid_frame_count < expected_vid_frame_count)
        {
            const uint16_t frame_id1 = get16(infile);
            const uint16_t frame_id2 = get16(infile);
            const uint32_t frame_size = get32(infile);

#ifdef VERBOSE_PRINT
            printf("\nframe id 0x%04"PRIx16",0x%04"PRIx16" ",frame_id1,frame_id2);
            printf("size 0x%08"PRIx32"\n", frame_size);
#endif

            if (frame_id1 == 1)
            {
                /* video */
                vid_frame_count ++;
#ifdef VERBOSE_PRINT
                printf("video frame %d/%d\n", (int)vid_frame_count, (int)expected_vid_frame_count);
#endif
                decode_video(&player, infile, total_vid_frames, frame_id2, frame_size);
            }
            else if (frame_id1 == 0)
            {
                /* audio */
                aud_frame_count ++;
                total_aud_frames ++;
                __attribute__((unused))
                const long audio_started = ftell(infile);
                const uint32_t samples = get32(infile);
                block_sample_count += samples;
#ifdef VERBOSE_PRINT
                printf("0x%lx: audio frame %d/%d (%d) (%d samples)\n", (unsigned long)audio_started, (int)aud_frame_count, (int)expected_aud_frame_count, (int)total_aud_frames, samples);
#endif
#if 0
                decode_audio(&audio_state, first_aud, samples, infile, outfile, header.audio_channels);
                first_aud = 0;
                long bytes_done_unto = ftell(infile) - audio_started;
                if (bytes_done_unto > frame_size)
                {
                    fprintf(stderr, "processed 0x%lx bytes, should have done 0x%"PRIx32"\n",
                        bytes_done_unto, frame_size);
                    exit(EXIT_FAILURE);
                }
                else if (bytes_done_unto < frame_size)
                {
                    while (bytes_done_unto < frame_size)
                    {
                        get8(infile);
                        bytes_done_unto ++;
                    }
                }
#else
                //printf("skipping\n");
                seek_past(frame_size - 4, infile);
#endif
            }
            else
            {
                fprintf(stderr, "unexpected frame id 0x%04X 0x%04X at %08lx\n", frame_id1, frame_id2, (unsigned long)(ftell(infile)-8));
                exit(EXIT_FAILURE);
                seek_past(frame_size, infile);
            }
        }
        puts("");
        free(audio_state.ch);

        if (expected_aud_frame_count != aud_frame_count ||
            expected_vid_frame_count != vid_frame_count)
        {
            fprintf(stderr, "frame count mismatch\n");
            exit(EXIT_FAILURE);
        }

        total_vid_frames += vid_frame_count;

#ifdef VERBOSE_PRINT
        printf("block %d ended at 0x%lx (%d samples)\n", (int)block_count, ftell(infile), block_sample_count);
#endif
        if (ftell(infile) != (data_start+(long)expected_block_size))
        {
            fprintf(stderr, "block size mismatch\n");
            exit(EXIT_FAILURE);
        }
        total_sample_count += block_sample_count;
    }

    free(player.seqobj.state);
    free(player.past);
    free(player.present);
    free(player.future);

    if (total_aud_frames != header.audio_frames ||
        total_vid_frames != header.video_frames)
    {
        fprintf(stderr, "total frame count mismatch\n");
        exit(EXIT_FAILURE);
    }

    printf("%"PRIu32" samples\n", total_sample_count);

    // generate header
    make_wav_header(riff_header, total_sample_count, header.audio_srate, header.audio_channels);
    fseek(outfile, 0, SEEK_SET);
    if (0x2c != fwrite(riff_header, 1, 0x2c, outfile))
    {
        fprintf(stderr, "error rewriting riff header\n");
        exit(EXIT_FAILURE);
    }
    if (EOF == fclose(outfile))
    {
        fprintf(stderr, "error finishing output\n");
        exit(EXIT_FAILURE);
    }

    printf("Done!\n");
}
#endif
