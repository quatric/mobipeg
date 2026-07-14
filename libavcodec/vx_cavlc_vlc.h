/*
 * Standard H.264 CAVLC residual coding VLC tables, as used by the
 * ActImagine / MobiClip DS (.vx) video codec. Generated mechanically from
 * package/vlc.py in https://github.com/CharlesVanEeckhout/actimagine so
 * the code/length data is guaranteed to match that reference bit-for-bit
 * (do not hand-edit; regenerate from vlc.py instead). Included only by
 * vx.c.
 */

static const int8_t vx_coeff_token_len[4][68] = {
    {1,0,0,0,6,2,0,0,8,6,3,0,9,8,7,5,10,9,8,6,11,10,9,7,13,11,10,8,13,13,11,9,13,13,13,10,14,14,13,11,14,14,14,13,15,15,14,14,15,15,15,14,16,15,15,15,16,16,16,15,16,16,16,16,16,16,16,16},
    {2,0,0,0,6,2,0,0,6,5,3,0,7,6,6,4,8,6,6,4,8,7,7,5,9,8,8,6,11,9,9,6,11,11,11,7,12,11,11,9,12,12,12,11,12,12,12,11,13,13,13,12,13,13,13,13,13,14,13,13,14,14,14,13,14,14,14,14},
    {4,0,0,0,6,4,0,0,6,5,4,0,6,5,5,4,7,5,5,4,7,5,5,4,7,6,6,4,7,6,6,4,8,7,7,5,8,8,7,6,9,8,8,7,9,9,8,8,9,9,9,8,10,9,9,9,10,10,10,10,10,10,10,10,10,10,10,10},
    {6,0,0,0,6,6,0,0,6,6,6,0,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6},
};
static const int8_t vx_coeff_token_bits[4][68] = {
    {1,0,0,0,5,1,0,0,7,4,1,0,7,6,5,3,7,6,5,3,7,6,5,4,15,6,5,4,11,14,5,4,8,10,13,4,15,14,9,4,11,10,13,12,15,14,9,12,11,10,13,8,15,1,9,12,11,14,13,8,7,10,9,12,4,6,5,8},
    {3,0,0,0,11,2,0,0,7,7,3,0,7,10,9,5,7,6,5,4,4,6,5,6,7,6,5,8,15,6,5,4,11,14,13,4,15,10,9,4,11,14,13,12,8,10,9,8,15,14,13,12,11,10,9,12,7,11,6,8,9,8,10,1,7,6,5,4},
    {15,0,0,0,15,14,0,0,11,15,13,0,8,12,14,12,15,10,11,11,11,8,9,10,9,14,13,9,8,10,9,8,15,14,13,13,11,14,10,12,15,10,13,12,11,14,9,12,8,10,13,8,13,7,9,12,9,12,11,10,5,8,7,6,1,4,3,2},
    {3,0,0,0,0,1,0,0,4,5,6,0,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63},
};
/* total_zeros: index 1..15 used */
static const int8_t vx_total_zeros_len_1[16] = {1,3,3,4,4,5,5,6,6,7,7,8,8,9,9,9};
static const int8_t vx_total_zeros_len_2[15] = {3,3,3,3,3,4,4,4,4,5,5,6,6,6,6};
static const int8_t vx_total_zeros_len_3[14] = {4,3,3,3,4,4,3,3,4,5,5,6,5,6};
static const int8_t vx_total_zeros_len_4[13] = {5,3,4,4,3,3,3,4,3,4,5,5,5};
static const int8_t vx_total_zeros_len_5[12] = {4,4,4,3,3,3,3,3,4,5,4,5};
static const int8_t vx_total_zeros_len_6[11] = {6,5,3,3,3,3,3,3,4,3,6};
static const int8_t vx_total_zeros_len_7[10] = {6,5,3,3,3,2,3,4,3,6};
static const int8_t vx_total_zeros_len_8[9] = {6,4,5,3,2,2,3,3,6};
static const int8_t vx_total_zeros_len_9[8] = {6,6,4,2,2,3,2,5};
static const int8_t vx_total_zeros_len_10[7] = {5,5,3,2,2,2,4};
static const int8_t vx_total_zeros_len_11[6] = {4,4,3,3,1,3};
static const int8_t vx_total_zeros_len_12[5] = {4,4,2,1,3};
static const int8_t vx_total_zeros_len_13[4] = {3,3,1,2};
static const int8_t vx_total_zeros_len_14[3] = {2,2,1};
static const int8_t vx_total_zeros_len_15[2] = {1,1};
static const int8_t vx_total_zeros_bits_1[16] = {1,3,2,3,2,3,2,3,2,3,2,3,2,3,2,1};
static const int8_t vx_total_zeros_bits_2[15] = {7,6,5,4,3,5,4,3,2,3,2,3,2,1,0};
static const int8_t vx_total_zeros_bits_3[14] = {5,7,6,5,4,3,4,3,2,3,2,1,1,0};
static const int8_t vx_total_zeros_bits_4[13] = {3,7,5,4,6,5,4,3,3,2,2,1,0};
static const int8_t vx_total_zeros_bits_5[12] = {5,4,3,7,6,5,4,3,2,1,1,0};
static const int8_t vx_total_zeros_bits_6[11] = {1,1,7,6,5,4,3,2,1,1,0};
static const int8_t vx_total_zeros_bits_7[10] = {1,1,5,4,3,3,2,1,1,0};
static const int8_t vx_total_zeros_bits_8[9] = {1,1,1,3,3,2,2,1,0};
static const int8_t vx_total_zeros_bits_9[8] = {1,0,1,3,2,1,1,1};
static const int8_t vx_total_zeros_bits_10[7] = {1,0,1,3,2,1,1};
static const int8_t vx_total_zeros_bits_11[6] = {0,1,1,2,1,3};
static const int8_t vx_total_zeros_bits_12[5] = {0,1,1,1,1};
static const int8_t vx_total_zeros_bits_13[4] = {0,1,1,1};
static const int8_t vx_total_zeros_bits_14[3] = {0,1,1};
static const int8_t vx_total_zeros_bits_15[2] = {0,1};
/* run: index 1..6 used */
static const int8_t vx_run_len_1[2] = {1,1};
static const int8_t vx_run_len_2[3] = {1,2,2};
static const int8_t vx_run_len_3[4] = {2,2,2,2};
static const int8_t vx_run_len_4[5] = {2,2,2,3,3};
static const int8_t vx_run_len_5[6] = {2,2,3,3,3,3};
static const int8_t vx_run_len_6[7] = {2,3,3,3,3,3,3};
static const int8_t vx_run_bits_1[2] = {1,0};
static const int8_t vx_run_bits_2[3] = {1,1,0};
static const int8_t vx_run_bits_3[4] = {3,2,1,0};
static const int8_t vx_run_bits_4[5] = {3,2,1,1,0};
static const int8_t vx_run_bits_5[6] = {3,2,3,2,1,0};
static const int8_t vx_run_bits_6[7] = {3,0,1,3,2,5,4};
static const int8_t vx_run7_len[15] = {3,3,3,3,3,3,3,4,5,6,7,8,9,10,11};
static const int8_t vx_run7_bits[15] = {7,6,5,4,3,2,1,1,1,1,1,1,1,1,1};
static const struct { const int8_t *len, *bits; int n; } vx_total_zeros_tabs[16] = {
    { NULL, NULL, 0 },
    { vx_total_zeros_len_1, vx_total_zeros_bits_1, 16 },
    { vx_total_zeros_len_2, vx_total_zeros_bits_2, 15 },
    { vx_total_zeros_len_3, vx_total_zeros_bits_3, 14 },
    { vx_total_zeros_len_4, vx_total_zeros_bits_4, 13 },
    { vx_total_zeros_len_5, vx_total_zeros_bits_5, 12 },
    { vx_total_zeros_len_6, vx_total_zeros_bits_6, 11 },
    { vx_total_zeros_len_7, vx_total_zeros_bits_7, 10 },
    { vx_total_zeros_len_8, vx_total_zeros_bits_8, 9 },
    { vx_total_zeros_len_9, vx_total_zeros_bits_9, 8 },
    { vx_total_zeros_len_10, vx_total_zeros_bits_10, 7 },
    { vx_total_zeros_len_11, vx_total_zeros_bits_11, 6 },
    { vx_total_zeros_len_12, vx_total_zeros_bits_12, 5 },
    { vx_total_zeros_len_13, vx_total_zeros_bits_13, 4 },
    { vx_total_zeros_len_14, vx_total_zeros_bits_14, 3 },
    { vx_total_zeros_len_15, vx_total_zeros_bits_15, 2 },
};
static const struct { const int8_t *len, *bits; int n; } vx_run_tabs[7] = {
    { NULL, NULL, 0 },
    { vx_run_len_1, vx_run_bits_1, 2 },
    { vx_run_len_2, vx_run_bits_2, 3 },
    { vx_run_len_3, vx_run_bits_3, 4 },
    { vx_run_len_4, vx_run_bits_4, 5 },
    { vx_run_len_5, vx_run_bits_5, 6 },
    { vx_run_len_6, vx_run_bits_6, 7 },
};
