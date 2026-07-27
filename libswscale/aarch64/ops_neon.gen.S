#include "libavutil/aarch64/asm.S"

function ff_sws_process_0001_neon, export=1, jumpable=0
        // prologue
        str             x30, [sp, #-16]!
        ldr             x7, [x1]                        // SwsFuncPtr op0_func = impl->cont;
        add             x8, x1, #32                     // SwsOpImpl *op1_impl = impl + 1;
        ldr             x9, [x0]                        // in[0] = exec->in[0];
        ldr             x10, [x0, #32]                  // out[0] = exec->out[0];
        ldr             x11, [x0, #128]                 // in_bump[0] = exec->in_bump[0];
        ldr             x12, [x0, #160]                 // out_bump[0] = exec->out_bump[0];
        b               2f                              // goto first_row;
1:                                                      // next_row:
        add             x9, x9, x11                     // in[0] += in_bump[0];
        add             x10, x10, x12                   // out[0] += out_bump[0];
2:                                                      // first_row:
        mov             w6, w2                          // bx = bx_start;
3:                                                      // next_block:
        mov             x1, x8                          // impl = op1_impl;
        blr             x7                              // op0_func();
        add             w6, w6, #1                      // bx += 1;
        cmp             w6, w4                          // if (bx != bx_end)
        b.ne            3b                              //     goto next_block;
        add             w3, w3, #1                      // y += 1;
        cmp             w3, w5                          // if (y != y_end)
        b.ne            1b                              //     goto next_row;
        // epilogue
        ldr             x30, [sp], #16
        ret
endfunc

function ff_sws_process_0011_neon, export=1, jumpable=0
        // prologue
        stp             x30, x19, [sp, #-16]!
        ldr             x7, [x1]                        // SwsFuncPtr op0_func = impl->cont;
        add             x8, x1, #32                     // SwsOpImpl *op1_impl = impl + 1;
        ldr             x9, [x0]                        // in[0] = exec->in[0];
        ldr             x13, [x0, #8]                   // in[1] = exec->in[1];
        ldr             x10, [x0, #32]                  // out[0] = exec->out[0];
        ldr             x14, [x0, #40]                  // out[1] = exec->out[1];
        ldr             x11, [x0, #128]                 // in_bump[0] = exec->in_bump[0];
        ldr             x15, [x0, #136]                 // in_bump[1] = exec->in_bump[1];
        ldr             x12, [x0, #160]                 // out_bump[0] = exec->out_bump[0];
        ldr             x19, [x0, #168]                 // out_bump[1] = exec->out_bump[1];
        b               2f                              // goto first_row;
1:                                                      // next_row:
        add             x9, x9, x11                     // in[0] += in_bump[0];
        add             x13, x13, x15                   // in[1] += in_bump[1];
        add             x10, x10, x12                   // out[0] += out_bump[0];
        add             x14, x14, x19                   // out[1] += out_bump[1];
2:                                                      // first_row:
        mov             w6, w2                          // bx = bx_start;
3:                                                      // next_block:
        mov             x1, x8                          // impl = op1_impl;
        blr             x7                              // op0_func();
        add             w6, w6, #1                      // bx += 1;
        cmp             w6, w4                          // if (bx != bx_end)
        b.ne            3b                              //     goto next_block;
        add             w3, w3, #1                      // y += 1;
        cmp             w3, w5                          // if (y != y_end)
        b.ne            1b                              //     goto next_row;
        // epilogue
        ldp             x30, x19, [sp], #16
        ret
endfunc

function ff_sws_process_0111_neon, export=1, jumpable=0
        // prologue
        stp             x30, x19, [sp, #-48]!
        stp             x20, x21, [sp, #16]
        stp             x22, x23, [sp, #32]
        ldr             x7, [x1]                        // SwsFuncPtr op0_func = impl->cont;
        add             x8, x1, #32                     // SwsOpImpl *op1_impl = impl + 1;
        ldr             x9, [x0]                        // in[0] = exec->in[0];
        ldr             x13, [x0, #8]                   // in[1] = exec->in[1];
        ldr             x20, [x0, #16]                  // in[2] = exec->in[2];
        ldr             x10, [x0, #32]                  // out[0] = exec->out[0];
        ldr             x14, [x0, #40]                  // out[1] = exec->out[1];
        ldr             x21, [x0, #48]                  // out[2] = exec->out[2];
        ldr             x11, [x0, #128]                 // in_bump[0] = exec->in_bump[0];
        ldr             x15, [x0, #136]                 // in_bump[1] = exec->in_bump[1];
        ldr             x22, [x0, #144]                 // in_bump[2] = exec->in_bump[2];
        ldr             x12, [x0, #160]                 // out_bump[0] = exec->out_bump[0];
        ldr             x19, [x0, #168]                 // out_bump[1] = exec->out_bump[1];
        ldr             x23, [x0, #176]                 // out_bump[2] = exec->out_bump[2];
        b               2f                              // goto first_row;
1:                                                      // next_row:
        add             x9, x9, x11                     // in[0] += in_bump[0];
        add             x13, x13, x15                   // in[1] += in_bump[1];
        add             x20, x20, x22                   // in[2] += in_bump[2];
        add             x10, x10, x12                   // out[0] += out_bump[0];
        add             x14, x14, x19                   // out[1] += out_bump[1];
        add             x21, x21, x23                   // out[2] += out_bump[2];
2:                                                      // first_row:
        mov             w6, w2                          // bx = bx_start;
3:                                                      // next_block:
        mov             x1, x8                          // impl = op1_impl;
        blr             x7                              // op0_func();
        add             w6, w6, #1                      // bx += 1;
        cmp             w6, w4                          // if (bx != bx_end)
        b.ne            3b                              //     goto next_block;
        add             w3, w3, #1                      // y += 1;
        cmp             w3, w5                          // if (y != y_end)
        b.ne            1b                              //     goto next_row;
        // epilogue
        ldp             x22, x23, [sp, #32]
        ldp             x20, x21, [sp, #16]
        ldp             x30, x19, [sp], #48
        ret
endfunc

function ff_sws_process_1111_neon, export=1, jumpable=0
        // prologue
        stp             x30, x19, [sp, #-80]!
        stp             x20, x21, [sp, #16]
        stp             x22, x23, [sp, #32]
        stp             x24, x25, [sp, #48]
        stp             x26, x27, [sp, #64]
        ldr             x7, [x1]                        // SwsFuncPtr op0_func = impl->cont;
        add             x8, x1, #32                     // SwsOpImpl *op1_impl = impl + 1;
        ldr             x9, [x0]                        // in[0] = exec->in[0];
        ldr             x13, [x0, #8]                   // in[1] = exec->in[1];
        ldr             x20, [x0, #16]                  // in[2] = exec->in[2];
        ldr             x24, [x0, #24]                  // in[3] = exec->in[3];
        ldr             x10, [x0, #32]                  // out[0] = exec->out[0];
        ldr             x14, [x0, #40]                  // out[1] = exec->out[1];
        ldr             x21, [x0, #48]                  // out[2] = exec->out[2];
        ldr             x25, [x0, #56]                  // out[3] = exec->out[3];
        ldr             x11, [x0, #128]                 // in_bump[0] = exec->in_bump[0];
        ldr             x15, [x0, #136]                 // in_bump[1] = exec->in_bump[1];
        ldr             x22, [x0, #144]                 // in_bump[2] = exec->in_bump[2];
        ldr             x26, [x0, #152]                 // in_bump[3] = exec->in_bump[3];
        ldr             x12, [x0, #160]                 // out_bump[0] = exec->out_bump[0];
        ldr             x19, [x0, #168]                 // out_bump[1] = exec->out_bump[1];
        ldr             x23, [x0, #176]                 // out_bump[2] = exec->out_bump[2];
        ldr             x27, [x0, #184]                 // out_bump[3] = exec->out_bump[3];
        b               2f                              // goto first_row;
1:                                                      // next_row:
        add             x9, x9, x11                     // in[0] += in_bump[0];
        add             x13, x13, x15                   // in[1] += in_bump[1];
        add             x20, x20, x22                   // in[2] += in_bump[2];
        add             x24, x24, x26                   // in[3] += in_bump[3];
        add             x10, x10, x12                   // out[0] += out_bump[0];
        add             x14, x14, x19                   // out[1] += out_bump[1];
        add             x21, x21, x23                   // out[2] += out_bump[2];
        add             x25, x25, x27                   // out[3] += out_bump[3];
2:                                                      // first_row:
        mov             w6, w2                          // bx = bx_start;
3:                                                      // next_block:
        mov             x1, x8                          // impl = op1_impl;
        blr             x7                              // op0_func();
        add             w6, w6, #1                      // bx += 1;
        cmp             w6, w4                          // if (bx != bx_end)
        b.ne            3b                              //     goto next_block;
        add             w3, w3, #1                      // y += 1;
        cmp             w3, w5                          // if (y != y_end)
        b.ne            1b                              //     goto next_row;
        // epilogue
        ldp             x26, x27, [sp, #64]
        ldp             x24, x25, [sp, #48]
        ldp             x22, x23, [sp, #32]
        ldp             x20, x21, [sp, #16]
        ldp             x30, x19, [sp], #80
        ret
endfunc

function ff_sws_read_bit_8_u8_0001_neon, export=1, jumpable=0
        ldr             q16, [x1, #16]                  // v128 shift_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ldrb            w16, [x9], #1                   // uint8_t tmp = *in[0]++;
        movi            v17.8b, #1                      // v128 bitmask_vec = {1 <repeats 8 times>, 0 <repeats 8 times>};
        dup             v0.8b, w16                      // vl[0].lo = broadcast(tmp);
        ushl            v0.8b, v0.8b, v16.8b            // vl[0] <<= shift_vec;
        and             v0.8b, v0.8b, v17.8b            // vl[0] &= bitmask_vec;
        br              x0                              // jump to cont
endfunc

function ff_sws_read_bit_16_u8_0001_neon, export=1, jumpable=0
        ldr             q16, [x1, #16]                  // v128 shift_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ldrh            w16, [x9], #2                   // uint16_t tmp = *in[0]++;
        movi            v17.16b, #1                     // v128 bitmask_vec = {1 <repeats 16 times>};
        dup             v0.8b, w16                      // vl[0].lo = broadcast(tmp);
        lsr             w16, w16, #8                    // tmp >>= 8;
        dup             v18.8b, w16                     // vtmp.lo = broadcast(tmp);
        ins             v0.d[1], v18.d[0]               // vl[0].hi = vtmp.lo;
        ushl            v0.16b, v0.16b, v16.16b         // vl[0] <<= shift_vec;
        and             v0.16b, v0.16b, v17.16b         // vl[0] &= bitmask_vec;
        br              x0                              // jump to cont
endfunc

function ff_sws_read_nibble_8_u8_0001_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        movi            v16.8b, #15                     // v128 nibble_mask = {0xf <repeats 8 times>, 0x0 <repeats 8 times>};
        ldr             s0, [x9], #4                    // vl[0] = *in[0]++;
        ushr            v17.8b, v0.8b, #4               // vtmp.lo = vl[0] >> 4;
        and             v0.8b, v0.8b, v16.8b            // vl[0].lo &= nibble_mask;
        zip1            v0.8b, v17.8b, v0.8b            // interleave
        br              x0                              // jump to cont
endfunc

function ff_sws_read_nibble_16_u8_0001_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        movi            v16.8b, #15                     // v128 nibble_mask = {0xf <repeats 8 times>, 0x0 <repeats 8 times>};
        ldr             d0, [x9], #8                    // vl[0] = *in[0]++;
        ushr            v17.8b, v0.8b, #4               // vtmp.lo = vl[0] >> 4;
        and             v0.8b, v0.8b, v16.8b            // vl[0].lo &= nibble_mask;
        zip1            v0.16b, v17.16b, v0.16b         // interleave
        br              x0                              // jump to cont
endfunc

function ff_sws_read_packed_8_u8_0011_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld2             {v0.8b, v1.8b}, [x9], #16
        br              x0                              // jump to cont
endfunc

function ff_sws_read_packed_8_u8_0111_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld3             {v0.8b, v1.8b, v2.8b}, [x9], #24
        br              x0                              // jump to cont
endfunc

function ff_sws_read_packed_8_u8_1111_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld4             {v0.8b, v1.8b, v2.8b, v3.8b}, [x9], #32
        br              x0                              // jump to cont
endfunc

function ff_sws_read_packed_8_u16_0011_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld2             {v0.8h, v1.8h}, [x9], #32
        br              x0                              // jump to cont
endfunc

function ff_sws_read_packed_8_u16_0111_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld3             {v0.8h, v1.8h, v2.8h}, [x9], #48
        br              x0                              // jump to cont
endfunc

function ff_sws_read_packed_8_u16_1111_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld4             {v0.8h, v1.8h, v2.8h, v3.8h}, [x9], #64
        br              x0                              // jump to cont
endfunc

function ff_sws_read_packed_8_u32_0011_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld2             {v0.4s, v1.4s}, [x9], #32
        ld2             {v4.4s, v5.4s}, [x9], #32
        br              x0                              // jump to cont
endfunc

function ff_sws_read_packed_8_u32_0111_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld3             {v0.4s, v1.4s, v2.4s}, [x9], #48
        ld3             {v4.4s, v5.4s, v6.4s}, [x9], #48
        br              x0                              // jump to cont
endfunc

function ff_sws_read_packed_8_u32_1111_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld4             {v0.4s, v1.4s, v2.4s, v3.4s}, [x9], #64
        ld4             {v4.4s, v5.4s, v6.4s, v7.4s}, [x9], #64
        br              x0                              // jump to cont
endfunc

function ff_sws_read_packed_16_u8_0011_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld2             {v0.16b, v1.16b}, [x9], #32
        br              x0                              // jump to cont
endfunc

function ff_sws_read_packed_16_u8_0111_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld3             {v0.16b, v1.16b, v2.16b}, [x9], #48
        br              x0                              // jump to cont
endfunc

function ff_sws_read_packed_16_u8_1111_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld4             {v0.16b, v1.16b, v2.16b, v3.16b}, [x9], #64
        br              x0                              // jump to cont
endfunc

function ff_sws_read_packed_16_u16_0011_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld2             {v0.8h, v1.8h}, [x9], #32
        ld2             {v4.8h, v5.8h}, [x9], #32
        br              x0                              // jump to cont
endfunc

function ff_sws_read_packed_16_u16_0111_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld3             {v0.8h, v1.8h, v2.8h}, [x9], #48
        ld3             {v4.8h, v5.8h, v6.8h}, [x9], #48
        br              x0                              // jump to cont
endfunc

function ff_sws_read_packed_16_u16_1111_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld4             {v0.8h, v1.8h, v2.8h, v3.8h}, [x9], #64
        ld4             {v4.8h, v5.8h, v6.8h, v7.8h}, [x9], #64
        br              x0                              // jump to cont
endfunc

function ff_sws_read_planar_8_u8_0001_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ldr             d0, [x9], #8
        br              x0                              // jump to cont
endfunc

function ff_sws_read_planar_8_u8_0011_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ldr             d0, [x9], #8
        ldr             d1, [x13], #8
        br              x0                              // jump to cont
endfunc

function ff_sws_read_planar_8_u8_0111_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ldr             d0, [x9], #8
        ldr             d1, [x13], #8
        ldr             d2, [x20], #8
        br              x0                              // jump to cont
endfunc

function ff_sws_read_planar_8_u8_1111_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ldr             d0, [x9], #8
        ldr             d1, [x13], #8
        ldr             d2, [x20], #8
        ldr             d3, [x24], #8
        br              x0                              // jump to cont
endfunc

function ff_sws_read_planar_8_u16_0001_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ldr             q0, [x9], #16
        br              x0                              // jump to cont
endfunc

function ff_sws_read_planar_8_u16_0011_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ldr             q0, [x9], #16
        ldr             q1, [x13], #16
        br              x0                              // jump to cont
endfunc

function ff_sws_read_planar_8_u16_0111_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ldr             q0, [x9], #16
        ldr             q1, [x13], #16
        ldr             q2, [x20], #16
        br              x0                              // jump to cont
endfunc

function ff_sws_read_planar_8_u16_1111_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ldr             q0, [x9], #16
        ldr             q1, [x13], #16
        ldr             q2, [x20], #16
        ldr             q3, [x24], #16
        br              x0                              // jump to cont
endfunc

function ff_sws_read_planar_8_u32_0001_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ldp             q0, q4, [x9], #32
        br              x0                              // jump to cont
endfunc

function ff_sws_read_planar_8_u32_0111_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ldp             q0, q4, [x9], #32
        ldp             q1, q5, [x13], #32
        ldp             q2, q6, [x20], #32
        br              x0                              // jump to cont
endfunc

function ff_sws_read_planar_8_u32_1111_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ldp             q0, q4, [x9], #32
        ldp             q1, q5, [x13], #32
        ldp             q2, q6, [x20], #32
        ldp             q3, q7, [x24], #32
        br              x0                              // jump to cont
endfunc

function ff_sws_read_planar_16_u8_0001_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ldr             q0, [x9], #16
        br              x0                              // jump to cont
endfunc

function ff_sws_read_planar_16_u8_0111_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ldr             q0, [x9], #16
        ldr             q1, [x13], #16
        ldr             q2, [x20], #16
        br              x0                              // jump to cont
endfunc

function ff_sws_read_planar_16_u8_1111_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ldr             q0, [x9], #16
        ldr             q1, [x13], #16
        ldr             q2, [x20], #16
        ldr             q3, [x24], #16
        br              x0                              // jump to cont
endfunc

function ff_sws_read_planar_16_u16_0001_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ldp             q0, q4, [x9], #32
        br              x0                              // jump to cont
endfunc

function ff_sws_read_planar_16_u16_0111_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ldp             q0, q4, [x9], #32
        ldp             q1, q5, [x13], #32
        ldp             q2, q6, [x20], #32
        br              x0                              // jump to cont
endfunc

function ff_sws_read_planar_16_u16_1111_neon, export=1, jumpable=0
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ldp             q0, q4, [x9], #32
        ldp             q1, q5, [x13], #32
        ldp             q2, q6, [x20], #32
        ldp             q3, q7, [x24], #32
        br              x0                              // jump to cont
endfunc

function ff_sws_write_bit_8_u8_0001_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 shift_vec = impl->priv.v128;
        ushl            v0.8b, v0.8b, v16.8b            // vl[0] <<= shift_vec;
        addv            b17, v0.8b                      // vtmp0[0] = add_across(vl[0].lo);
        str             b17, [x10], #1                  // *out[0]++ = vtmp0;
        ret
endfunc

function ff_sws_write_bit_16_u8_0001_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 shift_vec = impl->priv.v128;
        ushl            v0.16b, v0.16b, v16.16b         // vl[0] <<= shift_vec;
        addv            b17, v0.8b                      // vtmp0[0] = add_across(vl[0].lo);
        ins             v18.d[0], v0.d[1]               // vtmp1.lo = vl[0].hi;
        addv            b18, v18.8b                     // vtmp1[0] = add_across(vtmp1);
        ins             v17.b[1], v18.b[0]              // vtmp0[1] = vtmp1[0];
        str             h17, [x10], #2                  // *out[0]++ = vtmp0;
        ret
endfunc

function ff_sws_write_nibble_8_u8_0001_neon, export=1, jumpable=1
        shl             v16.4h, v0.4h, #4
        ushr            v17.4h, v0.4h, #8
        orr             v0.8b, v16.8b, v17.8b
        xtn             v16.8b, v0.8h
        str             s16, [x10], #4
        ret
endfunc

function ff_sws_write_nibble_16_u8_0001_neon, export=1, jumpable=1
        shl             v16.8h, v0.8h, #4
        ushr            v17.8h, v0.8h, #8
        orr             v0.16b, v16.16b, v17.16b
        xtn             v16.8b, v0.8h
        str             d16, [x10], #8
        ret
endfunc

function ff_sws_write_packed_8_u8_0011_neon, export=1, jumpable=1
        st2             {v0.8b, v1.8b}, [x10], #16
        ret
endfunc

function ff_sws_write_packed_8_u8_0111_neon, export=1, jumpable=1
        st3             {v0.8b, v1.8b, v2.8b}, [x10], #24
        ret
endfunc

function ff_sws_write_packed_8_u8_1111_neon, export=1, jumpable=1
        st4             {v0.8b, v1.8b, v2.8b, v3.8b}, [x10], #32
        ret
endfunc

function ff_sws_write_packed_8_u16_0011_neon, export=1, jumpable=1
        st2             {v0.8h, v1.8h}, [x10], #32
        ret
endfunc

function ff_sws_write_packed_8_u16_0111_neon, export=1, jumpable=1
        st3             {v0.8h, v1.8h, v2.8h}, [x10], #48
        ret
endfunc

function ff_sws_write_packed_8_u16_1111_neon, export=1, jumpable=1
        st4             {v0.8h, v1.8h, v2.8h, v3.8h}, [x10], #64
        ret
endfunc

function ff_sws_write_packed_8_u32_0011_neon, export=1, jumpable=1
        st2             {v0.4s, v1.4s}, [x10], #32
        st2             {v4.4s, v5.4s}, [x10], #32
        ret
endfunc

function ff_sws_write_packed_8_u32_0111_neon, export=1, jumpable=1
        st3             {v0.4s, v1.4s, v2.4s}, [x10], #48
        st3             {v4.4s, v5.4s, v6.4s}, [x10], #48
        ret
endfunc

function ff_sws_write_packed_8_u32_1111_neon, export=1, jumpable=1
        st4             {v0.4s, v1.4s, v2.4s, v3.4s}, [x10], #64
        st4             {v4.4s, v5.4s, v6.4s, v7.4s}, [x10], #64
        ret
endfunc

function ff_sws_write_packed_16_u8_0011_neon, export=1, jumpable=1
        st2             {v0.16b, v1.16b}, [x10], #32
        ret
endfunc

function ff_sws_write_packed_16_u8_0111_neon, export=1, jumpable=1
        st3             {v0.16b, v1.16b, v2.16b}, [x10], #48
        ret
endfunc

function ff_sws_write_packed_16_u8_1111_neon, export=1, jumpable=1
        st4             {v0.16b, v1.16b, v2.16b, v3.16b}, [x10], #64
        ret
endfunc

function ff_sws_write_packed_16_u16_0011_neon, export=1, jumpable=1
        st2             {v0.8h, v1.8h}, [x10], #32
        st2             {v4.8h, v5.8h}, [x10], #32
        ret
endfunc

function ff_sws_write_packed_16_u16_0111_neon, export=1, jumpable=1
        st3             {v0.8h, v1.8h, v2.8h}, [x10], #48
        st3             {v4.8h, v5.8h, v6.8h}, [x10], #48
        ret
endfunc

function ff_sws_write_packed_16_u16_1111_neon, export=1, jumpable=1
        st4             {v0.8h, v1.8h, v2.8h, v3.8h}, [x10], #64
        st4             {v4.8h, v5.8h, v6.8h, v7.8h}, [x10], #64
        ret
endfunc

function ff_sws_write_planar_8_u8_0001_neon, export=1, jumpable=1
        str             d0, [x10], #8
        ret
endfunc

function ff_sws_write_planar_8_u8_0111_neon, export=1, jumpable=1
        str             d0, [x10], #8
        str             d1, [x14], #8
        str             d2, [x21], #8
        ret
endfunc

function ff_sws_write_planar_8_u8_1111_neon, export=1, jumpable=1
        str             d0, [x10], #8
        str             d1, [x14], #8
        str             d2, [x21], #8
        str             d3, [x25], #8
        ret
endfunc

function ff_sws_write_planar_8_u16_0001_neon, export=1, jumpable=1
        str             q0, [x10], #16
        ret
endfunc

function ff_sws_write_planar_8_u16_0111_neon, export=1, jumpable=1
        str             q0, [x10], #16
        str             q1, [x14], #16
        str             q2, [x21], #16
        ret
endfunc

function ff_sws_write_planar_8_u16_1111_neon, export=1, jumpable=1
        str             q0, [x10], #16
        str             q1, [x14], #16
        str             q2, [x21], #16
        str             q3, [x25], #16
        ret
endfunc

function ff_sws_write_planar_8_u32_0001_neon, export=1, jumpable=1
        stp             q0, q4, [x10], #32
        ret
endfunc

function ff_sws_write_planar_8_u32_0111_neon, export=1, jumpable=1
        stp             q0, q4, [x10], #32
        stp             q1, q5, [x14], #32
        stp             q2, q6, [x21], #32
        ret
endfunc

function ff_sws_write_planar_8_u32_1111_neon, export=1, jumpable=1
        stp             q0, q4, [x10], #32
        stp             q1, q5, [x14], #32
        stp             q2, q6, [x21], #32
        stp             q3, q7, [x25], #32
        ret
endfunc

function ff_sws_write_planar_16_u8_0001_neon, export=1, jumpable=1
        str             q0, [x10], #16
        ret
endfunc

function ff_sws_write_planar_16_u8_0111_neon, export=1, jumpable=1
        str             q0, [x10], #16
        str             q1, [x14], #16
        str             q2, [x21], #16
        ret
endfunc

function ff_sws_write_planar_16_u8_1111_neon, export=1, jumpable=1
        str             q0, [x10], #16
        str             q1, [x14], #16
        str             q2, [x21], #16
        str             q3, [x25], #16
        ret
endfunc

function ff_sws_write_planar_16_u16_0001_neon, export=1, jumpable=1
        stp             q0, q4, [x10], #32
        ret
endfunc

function ff_sws_write_planar_16_u16_0111_neon, export=1, jumpable=1
        stp             q0, q4, [x10], #32
        stp             q1, q5, [x14], #32
        stp             q2, q6, [x21], #32
        ret
endfunc

function ff_sws_write_planar_16_u16_1111_neon, export=1, jumpable=1
        stp             q0, q4, [x10], #32
        stp             q1, q5, [x14], #32
        stp             q2, q6, [x21], #32
        stp             q3, q7, [x25], #32
        ret
endfunc

function ff_sws_swap_bytes_8_u16_0001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        rev16           v0.16b, v0.16b
        br              x0                              // jump to cont
endfunc

function ff_sws_swap_bytes_8_u16_0010_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        rev16           v1.16b, v1.16b
        br              x0                              // jump to cont
endfunc

function ff_sws_swap_bytes_8_u16_0011_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        rev16           v0.16b, v0.16b
        rev16           v1.16b, v1.16b
        br              x0                              // jump to cont
endfunc

function ff_sws_swap_bytes_8_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        rev16           v0.16b, v0.16b
        rev16           v1.16b, v1.16b
        rev16           v2.16b, v2.16b
        br              x0                              // jump to cont
endfunc

function ff_sws_swap_bytes_8_u16_1001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        rev16           v0.16b, v0.16b
        rev16           v3.16b, v3.16b
        br              x0                              // jump to cont
endfunc

function ff_sws_swap_bytes_8_u16_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        rev16           v1.16b, v1.16b
        rev16           v2.16b, v2.16b
        rev16           v3.16b, v3.16b
        br              x0                              // jump to cont
endfunc

function ff_sws_swap_bytes_8_u16_1111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        rev16           v0.16b, v0.16b
        rev16           v1.16b, v1.16b
        rev16           v2.16b, v2.16b
        rev16           v3.16b, v3.16b
        br              x0                              // jump to cont
endfunc

function ff_sws_swap_bytes_8_u32_0001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        rev32           v0.16b, v0.16b
        rev32           v4.16b, v4.16b
        br              x0                              // jump to cont
endfunc

function ff_sws_swap_bytes_8_u32_0011_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        rev32           v0.16b, v0.16b
        rev32           v1.16b, v1.16b
        rev32           v4.16b, v4.16b
        rev32           v5.16b, v5.16b
        br              x0                              // jump to cont
endfunc

function ff_sws_swap_bytes_8_u32_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        rev32           v0.16b, v0.16b
        rev32           v1.16b, v1.16b
        rev32           v2.16b, v2.16b
        rev32           v4.16b, v4.16b
        rev32           v5.16b, v5.16b
        rev32           v6.16b, v6.16b
        br              x0                              // jump to cont
endfunc

function ff_sws_swap_bytes_8_u32_1001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        rev32           v0.16b, v0.16b
        rev32           v3.16b, v3.16b
        rev32           v4.16b, v4.16b
        rev32           v7.16b, v7.16b
        br              x0                              // jump to cont
endfunc

function ff_sws_swap_bytes_8_u32_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        rev32           v1.16b, v1.16b
        rev32           v2.16b, v2.16b
        rev32           v3.16b, v3.16b
        rev32           v5.16b, v5.16b
        rev32           v6.16b, v6.16b
        rev32           v7.16b, v7.16b
        br              x0                              // jump to cont
endfunc

function ff_sws_swap_bytes_8_u32_1111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        rev32           v0.16b, v0.16b
        rev32           v1.16b, v1.16b
        rev32           v2.16b, v2.16b
        rev32           v3.16b, v3.16b
        rev32           v4.16b, v4.16b
        rev32           v5.16b, v5.16b
        rev32           v6.16b, v6.16b
        rev32           v7.16b, v7.16b
        br              x0                              // jump to cont
endfunc

function ff_sws_swap_bytes_16_u16_0001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        rev16           v0.16b, v0.16b
        rev16           v4.16b, v4.16b
        br              x0                              // jump to cont
endfunc

function ff_sws_swap_bytes_16_u16_0011_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        rev16           v0.16b, v0.16b
        rev16           v1.16b, v1.16b
        rev16           v4.16b, v4.16b
        rev16           v5.16b, v5.16b
        br              x0                              // jump to cont
endfunc

function ff_sws_swap_bytes_16_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        rev16           v0.16b, v0.16b
        rev16           v1.16b, v1.16b
        rev16           v2.16b, v2.16b
        rev16           v4.16b, v4.16b
        rev16           v5.16b, v5.16b
        rev16           v6.16b, v6.16b
        br              x0                              // jump to cont
endfunc

function ff_sws_swap_bytes_16_u16_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        rev16           v1.16b, v1.16b
        rev16           v2.16b, v2.16b
        rev16           v3.16b, v3.16b
        rev16           v5.16b, v5.16b
        rev16           v6.16b, v6.16b
        rev16           v7.16b, v7.16b
        br              x0                              // jump to cont
endfunc

function ff_sws_swap_bytes_16_u16_1111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        rev16           v0.16b, v0.16b
        rev16           v1.16b, v1.16b
        rev16           v2.16b, v2.16b
        rev16           v3.16b, v3.16b
        rev16           v4.16b, v4.16b
        rev16           v5.16b, v5.16b
        rev16           v6.16b, v6.16b
        rev16           v7.16b, v7.16b
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_0001_8_u8_1111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v2.8b, v0.8b                    // vl[2] = vl[0];
        mov             v3.8b, v0.8b                    // vl[3] = vl[0];
        mov             v16.8b, v0.8b                   // vtmpl = vl[0];
        mov             v0.8b, v1.8b                    // vl[0] = vl[1];
        mov             v1.8b, v16.8b                   // vl[1] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_0001_16_u8_1111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v2.16b, v0.16b                  // vl[2] = vl[0];
        mov             v3.16b, v0.16b                  // vl[3] = vl[0];
        mov             v16.16b, v0.16b                 // vtmpl = vl[0];
        mov             v0.16b, v1.16b                  // vl[0] = vl[1];
        mov             v1.16b, v16.16b                 // vl[1] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_000f_8_u8_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v1.8b, v0.8b                    // vl[1] = vl[0];
        mov             v2.8b, v0.8b                    // vl[2] = vl[0];
        mov             v3.8b, v0.8b                    // vl[3] = vl[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_000f_16_u8_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v1.16b, v0.16b                  // vl[1] = vl[0];
        mov             v2.16b, v0.16b                  // vl[2] = vl[0];
        mov             v3.16b, v0.16b                  // vl[3] = vl[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_000f_32_u8_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v1.16b, v0.16b                  // vl[1] = vl[0];
        mov             v5.16b, v4.16b                  // vh[1] = vh[0];
        mov             v2.16b, v0.16b                  // vl[2] = vl[0];
        mov             v6.16b, v4.16b                  // vh[2] = vh[0];
        mov             v3.16b, v0.16b                  // vl[3] = vl[0];
        mov             v7.16b, v4.16b                  // vh[3] = vh[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_0123_8_u8_1111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.8b, v0.8b                   // vtmpl = vl[0];
        mov             v0.8b, v3.8b                    // vl[0] = vl[3];
        mov             v3.8b, v16.8b                   // vl[3] = vtmpl;
        mov             v16.8b, v1.8b                   // vtmpl = vl[1];
        mov             v1.8b, v2.8b                    // vl[1] = vl[2];
        mov             v2.8b, v16.8b                   // vl[2] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_0123_16_u8_1111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.16b, v0.16b                 // vtmpl = vl[0];
        mov             v0.16b, v3.16b                  // vl[0] = vl[3];
        mov             v3.16b, v16.16b                 // vl[3] = vtmpl;
        mov             v16.16b, v1.16b                 // vtmpl = vl[1];
        mov             v1.16b, v2.16b                  // vl[1] = vl[2];
        mov             v2.16b, v16.16b                 // vl[2] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_0123_32_u8_1111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.16b, v0.16b                 // vtmpl = vl[0];
        mov             v17.16b, v4.16b                 // vtmph = vh[0];
        mov             v0.16b, v3.16b                  // vl[0] = vl[3];
        mov             v4.16b, v7.16b                  // vh[0] = vh[3];
        mov             v3.16b, v16.16b                 // vl[3] = vtmpl;
        mov             v7.16b, v17.16b                 // vh[3] = vtmph;
        mov             v16.16b, v1.16b                 // vtmpl = vl[1];
        mov             v17.16b, v5.16b                 // vtmph = vh[1];
        mov             v1.16b, v2.16b                  // vl[1] = vl[2];
        mov             v5.16b, v6.16b                  // vh[1] = vh[2];
        mov             v2.16b, v16.16b                 // vl[2] = vtmpl;
        mov             v6.16b, v17.16b                 // vh[2] = vtmph;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_012f_8_u8_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v3.8b, v0.8b                    // vl[3] = vl[0];
        mov             v16.8b, v1.8b                   // vtmpl = vl[1];
        mov             v1.8b, v2.8b                    // vl[1] = vl[2];
        mov             v2.8b, v16.8b                   // vl[2] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_012f_16_u8_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v3.16b, v0.16b                  // vl[3] = vl[0];
        mov             v16.16b, v1.16b                 // vtmpl = vl[1];
        mov             v1.16b, v2.16b                  // vl[1] = vl[2];
        mov             v2.16b, v16.16b                 // vl[2] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_012f_32_u8_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v3.16b, v0.16b                  // vl[3] = vl[0];
        mov             v7.16b, v4.16b                  // vh[3] = vh[0];
        mov             v16.16b, v1.16b                 // vtmpl = vl[1];
        mov             v17.16b, v5.16b                 // vtmph = vh[1];
        mov             v1.16b, v2.16b                  // vl[1] = vl[2];
        mov             v5.16b, v6.16b                  // vh[1] = vh[2];
        mov             v2.16b, v16.16b                 // vl[2] = vtmpl;
        mov             v6.16b, v17.16b                 // vh[2] = vtmph;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_0321_16_u8_1111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.16b, v0.16b                 // vtmpl = vl[0];
        mov             v0.16b, v1.16b                  // vl[0] = vl[1];
        mov             v1.16b, v2.16b                  // vl[1] = vl[2];
        mov             v2.16b, v3.16b                  // vl[2] = vl[3];
        mov             v3.16b, v16.16b                 // vl[3] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_0321_32_u8_1111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.16b, v0.16b                 // vtmpl = vl[0];
        mov             v17.16b, v4.16b                 // vtmph = vh[0];
        mov             v0.16b, v1.16b                  // vl[0] = vl[1];
        mov             v4.16b, v5.16b                  // vh[0] = vh[1];
        mov             v1.16b, v2.16b                  // vl[1] = vl[2];
        mov             v5.16b, v6.16b                  // vh[1] = vh[2];
        mov             v2.16b, v3.16b                  // vl[2] = vl[3];
        mov             v6.16b, v7.16b                  // vh[2] = vh[3];
        mov             v3.16b, v16.16b                 // vl[3] = vtmpl;
        mov             v7.16b, v17.16b                 // vh[3] = vtmph;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_03f2_16_u8_1101_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.16b, v0.16b                 // vtmpl = vl[0];
        mov             v0.16b, v2.16b                  // vl[0] = vl[2];
        mov             v2.16b, v3.16b                  // vl[2] = vl[3];
        mov             v3.16b, v16.16b                 // vl[3] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_0ff1_32_u8_1001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v3.16b, v0.16b                  // vl[3] = vl[0];
        mov             v7.16b, v4.16b                  // vh[3] = vh[0];
        mov             v0.16b, v1.16b                  // vl[0] = vl[1];
        mov             v4.16b, v5.16b                  // vh[0] = vh[1];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_0fff_8_u8_1000_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v3.8b, v0.8b                    // vl[3] = vl[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_0fff_16_u8_1000_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v3.16b, v0.16b                  // vl[3] = vl[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_0fff_32_u8_1000_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v3.16b, v0.16b                  // vl[3] = vl[0];
        mov             v7.16b, v4.16b                  // vh[3] = vh[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_100f_8_u8_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v2.8b, v0.8b                    // vl[2] = vl[0];
        mov             v3.8b, v1.8b                    // vl[3] = vl[1];
        mov             v1.8b, v0.8b                    // vl[1] = vl[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_100f_16_u8_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v2.16b, v0.16b                  // vl[2] = vl[0];
        mov             v3.16b, v1.16b                  // vl[3] = vl[1];
        mov             v1.16b, v0.16b                  // vl[1] = vl[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_100f_32_u8_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v2.16b, v0.16b                  // vl[2] = vl[0];
        mov             v6.16b, v4.16b                  // vh[2] = vh[0];
        mov             v3.16b, v1.16b                  // vl[3] = vl[1];
        mov             v7.16b, v5.16b                  // vh[3] = vh[1];
        mov             v1.16b, v0.16b                  // vl[1] = vl[0];
        mov             v5.16b, v4.16b                  // vh[1] = vh[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_1023_8_u8_1111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.8b, v0.8b                   // vtmpl = vl[0];
        mov             v0.8b, v3.8b                    // vl[0] = vl[3];
        mov             v3.8b, v1.8b                    // vl[3] = vl[1];
        mov             v1.8b, v2.8b                    // vl[1] = vl[2];
        mov             v2.8b, v16.8b                   // vl[2] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_1023_32_u8_1111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.16b, v0.16b                 // vtmpl = vl[0];
        mov             v17.16b, v4.16b                 // vtmph = vh[0];
        mov             v0.16b, v3.16b                  // vl[0] = vl[3];
        mov             v4.16b, v7.16b                  // vh[0] = vh[3];
        mov             v3.16b, v1.16b                  // vl[3] = vl[1];
        mov             v7.16b, v5.16b                  // vh[3] = vh[1];
        mov             v1.16b, v2.16b                  // vl[1] = vl[2];
        mov             v5.16b, v6.16b                  // vh[1] = vh[2];
        mov             v2.16b, v16.16b                 // vl[2] = vtmpl;
        mov             v6.16b, v17.16b                 // vh[2] = vtmph;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_102f_8_u8_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v3.8b, v1.8b                    // vl[3] = vl[1];
        mov             v1.8b, v2.8b                    // vl[1] = vl[2];
        mov             v2.8b, v0.8b                    // vl[2] = vl[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_102f_32_u8_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v3.16b, v1.16b                  // vl[3] = vl[1];
        mov             v7.16b, v5.16b                  // vh[3] = vh[1];
        mov             v1.16b, v2.16b                  // vl[1] = vl[2];
        mov             v5.16b, v6.16b                  // vh[1] = vh[2];
        mov             v2.16b, v0.16b                  // vl[2] = vl[0];
        mov             v6.16b, v4.16b                  // vh[2] = vh[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_132f_8_u8_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.8b, v1.8b                   // vtmpl = vl[1];
        mov             v1.8b, v2.8b                    // vl[1] = vl[2];
        mov             v2.8b, v3.8b                    // vl[2] = vl[3];
        mov             v3.8b, v16.8b                   // vl[3] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_132f_16_u8_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.16b, v1.16b                 // vtmpl = vl[1];
        mov             v1.16b, v2.16b                  // vl[1] = vl[2];
        mov             v2.16b, v3.16b                  // vl[2] = vl[3];
        mov             v3.16b, v16.16b                 // vl[3] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_1f0f_32_u8_1010_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v3.16b, v1.16b                  // vl[3] = vl[1];
        mov             v7.16b, v5.16b                  // vh[3] = vh[1];
        mov             v1.16b, v0.16b                  // vl[1] = vl[0];
        mov             v5.16b, v4.16b                  // vh[1] = vh[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_1f3f_8_u8_1010_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.8b, v1.8b                   // vtmpl = vl[1];
        mov             v1.8b, v3.8b                    // vl[1] = vl[3];
        mov             v3.8b, v16.8b                   // vl[3] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_1f3f_16_u8_1010_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.16b, v1.16b                 // vtmpl = vl[1];
        mov             v1.16b, v3.16b                  // vl[1] = vl[3];
        mov             v3.16b, v16.16b                 // vl[3] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_1f3f_32_u8_1010_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.16b, v1.16b                 // vtmpl = vl[1];
        mov             v17.16b, v5.16b                 // vtmph = vh[1];
        mov             v1.16b, v3.16b                  // vl[1] = vl[3];
        mov             v5.16b, v7.16b                  // vh[1] = vh[3];
        mov             v3.16b, v16.16b                 // vl[3] = vtmpl;
        mov             v7.16b, v17.16b                 // vh[3] = vtmph;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_1fff_32_u8_1000_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v3.16b, v1.16b                  // vl[3] = vl[1];
        mov             v7.16b, v5.16b                  // vh[3] = vh[1];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_20f3_8_u8_1101_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.8b, v0.8b                   // vtmpl = vl[0];
        mov             v0.8b, v3.8b                    // vl[0] = vl[3];
        mov             v3.8b, v2.8b                    // vl[3] = vl[2];
        mov             v2.8b, v16.8b                   // vl[2] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_20f3_16_u8_1101_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.16b, v0.16b                 // vtmpl = vl[0];
        mov             v0.16b, v3.16b                  // vl[0] = vl[3];
        mov             v3.16b, v2.16b                  // vl[3] = vl[2];
        mov             v2.16b, v16.16b                 // vl[2] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_20ff_8_u8_1100_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v3.8b, v2.8b                    // vl[3] = vl[2];
        mov             v2.8b, v0.8b                    // vl[2] = vl[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_20ff_32_u8_1100_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v3.16b, v2.16b                  // vl[3] = vl[2];
        mov             v7.16b, v6.16b                  // vh[3] = vh[2];
        mov             v2.16b, v0.16b                  // vl[2] = vl[0];
        mov             v6.16b, v4.16b                  // vh[2] = vh[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_2103_8_u8_1111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.8b, v0.8b                   // vtmpl = vl[0];
        mov             v0.8b, v3.8b                    // vl[0] = vl[3];
        mov             v3.8b, v2.8b                    // vl[3] = vl[2];
        mov             v2.8b, v1.8b                    // vl[2] = vl[1];
        mov             v1.8b, v16.8b                   // vl[1] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_2103_16_u8_1111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.16b, v0.16b                 // vtmpl = vl[0];
        mov             v0.16b, v3.16b                  // vl[0] = vl[3];
        mov             v3.16b, v2.16b                  // vl[3] = vl[2];
        mov             v2.16b, v1.16b                  // vl[2] = vl[1];
        mov             v1.16b, v16.16b                 // vl[1] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_2103_32_u8_1111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.16b, v0.16b                 // vtmpl = vl[0];
        mov             v17.16b, v4.16b                 // vtmph = vh[0];
        mov             v0.16b, v3.16b                  // vl[0] = vl[3];
        mov             v4.16b, v7.16b                  // vh[0] = vh[3];
        mov             v3.16b, v2.16b                  // vl[3] = vl[2];
        mov             v7.16b, v6.16b                  // vh[3] = vh[2];
        mov             v2.16b, v1.16b                  // vl[2] = vl[1];
        mov             v6.16b, v5.16b                  // vh[2] = vh[1];
        mov             v1.16b, v16.16b                 // vl[1] = vtmpl;
        mov             v5.16b, v17.16b                 // vh[1] = vtmph;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_210f_8_u8_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v3.8b, v2.8b                    // vl[3] = vl[2];
        mov             v2.8b, v1.8b                    // vl[2] = vl[1];
        mov             v1.8b, v0.8b                    // vl[1] = vl[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_210f_16_u8_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v3.16b, v2.16b                  // vl[3] = vl[2];
        mov             v2.16b, v1.16b                  // vl[2] = vl[1];
        mov             v1.16b, v0.16b                  // vl[1] = vl[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_210f_32_u8_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v3.16b, v2.16b                  // vl[3] = vl[2];
        mov             v7.16b, v6.16b                  // vh[3] = vh[2];
        mov             v2.16b, v1.16b                  // vl[2] = vl[1];
        mov             v6.16b, v5.16b                  // vh[2] = vh[1];
        mov             v1.16b, v0.16b                  // vl[1] = vl[0];
        mov             v5.16b, v4.16b                  // vh[1] = vh[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f00f_8_u8_0110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v1.8b, v0.8b                    // vl[1] = vl[0];
        mov             v2.8b, v0.8b                    // vl[2] = vl[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f00f_16_u8_0110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v1.16b, v0.16b                  // vl[1] = vl[0];
        mov             v2.16b, v0.16b                  // vl[2] = vl[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f00f_32_u8_0110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v1.16b, v0.16b                  // vl[1] = vl[0];
        mov             v5.16b, v4.16b                  // vh[1] = vh[0];
        mov             v2.16b, v0.16b                  // vl[2] = vl[0];
        mov             v6.16b, v4.16b                  // vh[2] = vh[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f021_8_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.8b, v0.8b                   // vtmpl = vl[0];
        mov             v0.8b, v1.8b                    // vl[0] = vl[1];
        mov             v1.8b, v2.8b                    // vl[1] = vl[2];
        mov             v2.8b, v16.8b                   // vl[2] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f021_16_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.16b, v0.16b                 // vtmpl = vl[0];
        mov             v0.16b, v1.16b                  // vl[0] = vl[1];
        mov             v1.16b, v2.16b                  // vl[1] = vl[2];
        mov             v2.16b, v16.16b                 // vl[2] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f021_32_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.16b, v0.16b                 // vtmpl = vl[0];
        mov             v17.16b, v4.16b                 // vtmph = vh[0];
        mov             v0.16b, v1.16b                  // vl[0] = vl[1];
        mov             v4.16b, v5.16b                  // vh[0] = vh[1];
        mov             v1.16b, v2.16b                  // vl[1] = vl[2];
        mov             v5.16b, v6.16b                  // vh[1] = vh[2];
        mov             v2.16b, v16.16b                 // vl[2] = vtmpl;
        mov             v6.16b, v17.16b                 // vh[2] = vtmph;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f0f2_8_u8_0101_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.8b, v0.8b                   // vtmpl = vl[0];
        mov             v0.8b, v2.8b                    // vl[0] = vl[2];
        mov             v2.8b, v16.8b                   // vl[2] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f0f2_16_u8_0101_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.16b, v0.16b                 // vtmpl = vl[0];
        mov             v0.16b, v2.16b                  // vl[0] = vl[2];
        mov             v2.16b, v16.16b                 // vl[2] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f0f2_32_u8_0101_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.16b, v0.16b                 // vtmpl = vl[0];
        mov             v17.16b, v4.16b                 // vtmph = vh[0];
        mov             v0.16b, v2.16b                  // vl[0] = vl[2];
        mov             v4.16b, v6.16b                  // vh[0] = vh[2];
        mov             v2.16b, v16.16b                 // vl[2] = vtmpl;
        mov             v6.16b, v17.16b                 // vh[2] = vtmph;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f0f3_32_u8_0101_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v2.16b, v0.16b                  // vl[2] = vl[0];
        mov             v6.16b, v4.16b                  // vh[2] = vh[0];
        mov             v0.16b, v3.16b                  // vl[0] = vl[3];
        mov             v4.16b, v7.16b                  // vh[0] = vh[3];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f0ff_8_u8_0100_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v2.8b, v0.8b                    // vl[2] = vl[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f0ff_32_u8_0100_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v2.16b, v0.16b                  // vl[2] = vl[0];
        mov             v6.16b, v4.16b                  // vh[2] = vh[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f102_8_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.8b, v0.8b                   // vtmpl = vl[0];
        mov             v0.8b, v2.8b                    // vl[0] = vl[2];
        mov             v2.8b, v1.8b                    // vl[2] = vl[1];
        mov             v1.8b, v16.8b                   // vl[1] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f102_16_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.16b, v0.16b                 // vtmpl = vl[0];
        mov             v0.16b, v2.16b                  // vl[0] = vl[2];
        mov             v2.16b, v1.16b                  // vl[2] = vl[1];
        mov             v1.16b, v16.16b                 // vl[1] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f102_32_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.16b, v0.16b                 // vtmpl = vl[0];
        mov             v17.16b, v4.16b                 // vtmph = vh[0];
        mov             v0.16b, v2.16b                  // vl[0] = vl[2];
        mov             v4.16b, v6.16b                  // vh[0] = vh[2];
        mov             v2.16b, v1.16b                  // vl[2] = vl[1];
        mov             v6.16b, v5.16b                  // vh[2] = vh[1];
        mov             v1.16b, v16.16b                 // vl[1] = vtmpl;
        mov             v5.16b, v17.16b                 // vh[1] = vtmph;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f123_8_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v0.8b, v3.8b                    // vl[0] = vl[3];
        mov             v16.8b, v1.8b                   // vtmpl = vl[1];
        mov             v1.8b, v2.8b                    // vl[1] = vl[2];
        mov             v2.8b, v16.8b                   // vl[2] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f123_16_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v0.16b, v3.16b                  // vl[0] = vl[3];
        mov             v16.16b, v1.16b                 // vtmpl = vl[1];
        mov             v1.16b, v2.16b                  // vl[1] = vl[2];
        mov             v2.16b, v16.16b                 // vl[2] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f123_32_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v0.16b, v3.16b                  // vl[0] = vl[3];
        mov             v4.16b, v7.16b                  // vh[0] = vh[3];
        mov             v16.16b, v1.16b                 // vtmpl = vl[1];
        mov             v17.16b, v5.16b                 // vtmph = vh[1];
        mov             v1.16b, v2.16b                  // vl[1] = vl[2];
        mov             v5.16b, v6.16b                  // vh[1] = vh[2];
        mov             v2.16b, v16.16b                 // vl[2] = vtmpl;
        mov             v6.16b, v17.16b                 // vh[2] = vtmph;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f12f_8_u8_0110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.8b, v1.8b                   // vtmpl = vl[1];
        mov             v1.8b, v2.8b                    // vl[1] = vl[2];
        mov             v2.8b, v16.8b                   // vl[2] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f12f_16_u8_0110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.16b, v1.16b                 // vtmpl = vl[1];
        mov             v1.16b, v2.16b                  // vl[1] = vl[2];
        mov             v2.16b, v16.16b                 // vl[2] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f12f_32_u8_0110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.16b, v1.16b                 // vtmpl = vl[1];
        mov             v17.16b, v5.16b                 // vtmph = vh[1];
        mov             v1.16b, v2.16b                  // vl[1] = vl[2];
        mov             v5.16b, v6.16b                  // vh[1] = vh[2];
        mov             v2.16b, v16.16b                 // vl[2] = vtmpl;
        mov             v6.16b, v17.16b                 // vh[2] = vtmph;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f132_8_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v0.8b, v2.8b                    // vl[0] = vl[2];
        mov             v2.8b, v1.8b                    // vl[2] = vl[1];
        mov             v1.8b, v3.8b                    // vl[1] = vl[3];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f132_16_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v0.16b, v2.16b                  // vl[0] = vl[2];
        mov             v2.16b, v1.16b                  // vl[2] = vl[1];
        mov             v1.16b, v3.16b                  // vl[1] = vl[3];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f132_32_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v0.16b, v2.16b                  // vl[0] = vl[2];
        mov             v4.16b, v6.16b                  // vh[0] = vh[2];
        mov             v2.16b, v1.16b                  // vl[2] = vl[1];
        mov             v6.16b, v5.16b                  // vh[2] = vh[1];
        mov             v1.16b, v3.16b                  // vl[1] = vl[3];
        mov             v5.16b, v7.16b                  // vh[1] = vh[3];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f321_8_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v0.8b, v1.8b                    // vl[0] = vl[1];
        mov             v1.8b, v2.8b                    // vl[1] = vl[2];
        mov             v2.8b, v3.8b                    // vl[2] = vl[3];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f321_16_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v0.16b, v1.16b                  // vl[0] = vl[1];
        mov             v1.16b, v2.16b                  // vl[1] = vl[2];
        mov             v2.16b, v3.16b                  // vl[2] = vl[3];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f321_32_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v0.16b, v1.16b                  // vl[0] = vl[1];
        mov             v4.16b, v5.16b                  // vh[0] = vh[1];
        mov             v1.16b, v2.16b                  // vl[1] = vl[2];
        mov             v5.16b, v6.16b                  // vh[1] = vh[2];
        mov             v2.16b, v3.16b                  // vl[2] = vl[3];
        mov             v6.16b, v7.16b                  // vh[2] = vh[3];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f3f2_16_u8_0101_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v0.16b, v2.16b                  // vl[0] = vl[2];
        mov             v2.16b, v3.16b                  // vl[2] = vl[3];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_f3f2_32_u8_0101_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v0.16b, v2.16b                  // vl[0] = vl[2];
        mov             v4.16b, v6.16b                  // vh[0] = vh[2];
        mov             v2.16b, v3.16b                  // vl[2] = vl[3];
        mov             v6.16b, v7.16b                  // vh[2] = vh[3];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_ff01_8_u8_0011_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.8b, v0.8b                   // vtmpl = vl[0];
        mov             v0.8b, v1.8b                    // vl[0] = vl[1];
        mov             v1.8b, v16.8b                   // vl[1] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_ff01_16_u8_0011_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.16b, v0.16b                 // vtmpl = vl[0];
        mov             v0.16b, v1.16b                  // vl[0] = vl[1];
        mov             v1.16b, v16.16b                 // vl[1] = vtmpl;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_ff01_32_u8_0011_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v16.16b, v0.16b                 // vtmpl = vl[0];
        mov             v17.16b, v4.16b                 // vtmph = vh[0];
        mov             v0.16b, v1.16b                  // vl[0] = vl[1];
        mov             v4.16b, v5.16b                  // vh[0] = vh[1];
        mov             v1.16b, v16.16b                 // vl[1] = vtmpl;
        mov             v5.16b, v17.16b                 // vh[1] = vtmph;
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_ff03_8_u8_0011_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v1.8b, v0.8b                    // vl[1] = vl[0];
        mov             v0.8b, v3.8b                    // vl[0] = vl[3];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_ff03_16_u8_0011_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v1.16b, v0.16b                  // vl[1] = vl[0];
        mov             v0.16b, v3.16b                  // vl[0] = vl[3];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_ff0f_8_u8_0010_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v1.8b, v0.8b                    // vl[1] = vl[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_ff0f_16_u8_0010_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v1.16b, v0.16b                  // vl[1] = vl[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_ff0f_32_u8_0010_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v1.16b, v0.16b                  // vl[1] = vl[0];
        mov             v5.16b, v4.16b                  // vh[1] = vh[0];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_ff31_8_u8_0011_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v0.8b, v1.8b                    // vl[0] = vl[1];
        mov             v1.8b, v3.8b                    // vl[1] = vl[3];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_ff3f_8_u8_0010_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v1.8b, v3.8b                    // vl[1] = vl[3];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_ff3f_16_u8_0010_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v1.16b, v3.16b                  // vl[1] = vl[3];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_ff3f_32_u8_0010_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v1.16b, v3.16b                  // vl[1] = vl[3];
        mov             v5.16b, v7.16b                  // vh[1] = vh[3];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_fff1_32_u8_0001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v0.16b, v1.16b                  // vl[0] = vl[1];
        mov             v4.16b, v5.16b                  // vh[0] = vh[1];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_fff2_32_u8_0001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v0.16b, v2.16b                  // vl[0] = vl[2];
        mov             v4.16b, v6.16b                  // vh[0] = vh[2];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_fff3_8_u8_0001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v0.8b, v3.8b                    // vl[0] = vl[3];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_fff3_16_u8_0001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v0.16b, v3.16b                  // vl[0] = vl[3];
        br              x0                              // jump to cont
endfunc

function ff_sws_swizzle_fff3_32_u8_0001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        mov             v0.16b, v3.16b                  // vl[0] = vl[3];
        mov             v4.16b, v7.16b                  // vh[0] = vh[3];
        br              x0                              // jump to cont
endfunc

function ff_sws_unpack_0121_8_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // generate masks
        movi            v16.8b, #1
        movi            v17.8b, #3
        mov             v2.16b, v0.16b                  // vl[2] = vl[0];
        ushr            v1.8b, v0.8b, #1                // vl[1] >>= 1;
        ushr            v0.8b, v0.8b, #3                // vl[0] >>= 3;
        and             v2.16b, v2.16b, v16.16b         // vl[2] &= 0x1;
        and             v1.16b, v1.16b, v17.16b         // vl[1] &= 0x3;
        and             v0.16b, v0.16b, v16.16b         // vl[0] &= 0x1;
        br              x0                              // jump to cont
endfunc

function ff_sws_unpack_0121_16_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // generate masks
        movi            v16.16b, #1
        movi            v17.16b, #3
        mov             v2.16b, v0.16b                  // vl[2] = vl[0];
        ushr            v1.16b, v0.16b, #1              // vl[1] >>= 1;
        ushr            v0.16b, v0.16b, #3              // vl[0] >>= 3;
        and             v2.16b, v2.16b, v16.16b         // vl[2] &= 0x1;
        and             v1.16b, v1.16b, v17.16b         // vl[1] &= 0x3;
        and             v0.16b, v0.16b, v16.16b         // vl[0] &= 0x1;
        br              x0                              // jump to cont
endfunc

function ff_sws_unpack_0233_8_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // generate masks
        movi            v16.8b, #7
        movi            v17.8b, #3
        mov             v2.16b, v0.16b                  // vl[2] = vl[0];
        ushr            v1.8b, v0.8b, #2                // vl[1] >>= 2;
        ushr            v0.8b, v0.8b, #5                // vl[0] >>= 5;
        and             v2.16b, v2.16b, v17.16b         // vl[2] &= 0x3;
        and             v1.16b, v1.16b, v16.16b         // vl[1] &= 0x7;
        and             v0.16b, v0.16b, v16.16b         // vl[0] &= 0x7;
        br              x0                              // jump to cont
endfunc

function ff_sws_unpack_0233_16_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // generate masks
        movi            v16.16b, #7
        movi            v17.16b, #3
        mov             v2.16b, v0.16b                  // vl[2] = vl[0];
        ushr            v1.16b, v0.16b, #2              // vl[1] >>= 2;
        ushr            v0.16b, v0.16b, #5              // vl[0] >>= 5;
        and             v2.16b, v2.16b, v17.16b         // vl[2] &= 0x3;
        and             v1.16b, v1.16b, v16.16b         // vl[1] &= 0x7;
        and             v0.16b, v0.16b, v16.16b         // vl[0] &= 0x7;
        br              x0                              // jump to cont
endfunc

function ff_sws_unpack_0332_8_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // generate masks
        movi            v16.8b, #3
        movi            v17.8b, #7
        mov             v2.16b, v0.16b                  // vl[2] = vl[0];
        ushr            v1.8b, v0.8b, #3                // vl[1] >>= 3;
        ushr            v0.8b, v0.8b, #6                // vl[0] >>= 6;
        and             v2.16b, v2.16b, v17.16b         // vl[2] &= 0x7;
        and             v1.16b, v1.16b, v17.16b         // vl[1] &= 0x7;
        and             v0.16b, v0.16b, v16.16b         // vl[0] &= 0x3;
        br              x0                              // jump to cont
endfunc

function ff_sws_unpack_0332_16_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // generate masks
        movi            v16.16b, #3
        movi            v17.16b, #7
        mov             v2.16b, v0.16b                  // vl[2] = vl[0];
        ushr            v1.16b, v0.16b, #3              // vl[1] >>= 3;
        ushr            v0.16b, v0.16b, #6              // vl[0] >>= 6;
        and             v2.16b, v2.16b, v17.16b         // vl[2] &= 0x7;
        and             v1.16b, v1.16b, v17.16b         // vl[1] &= 0x7;
        and             v0.16b, v0.16b, v16.16b         // vl[0] &= 0x3;
        br              x0                              // jump to cont
endfunc

function ff_sws_unpack_0444_8_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // generate masks
        movi            v16.8h, #15
        mov             v2.16b, v0.16b                  // vl[2] = vl[0];
        ushr            v1.8h, v0.8h, #4                // vl[1] >>= 4;
        ushr            v0.8h, v0.8h, #8                // vl[0] >>= 8;
        and             v2.16b, v2.16b, v16.16b         // vl[2] &= 0xf;
        and             v1.16b, v1.16b, v16.16b         // vl[1] &= 0xf;
        and             v0.16b, v0.16b, v16.16b         // vl[0] &= 0xf;
        br              x0                              // jump to cont
endfunc

function ff_sws_unpack_0444_16_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // generate masks
        movi            v16.8h, #15
        mov             v2.16b, v0.16b                  // vl[2] = vl[0];
        ushr            v1.8h, v0.8h, #4                // vl[1] >>= 4;
        ushr            v0.8h, v0.8h, #8                // vl[0] >>= 8;
        mov             v6.16b, v4.16b                  // vh[2] = vh[0];
        ushr            v5.8h, v4.8h, #4                // vh[1] >>= 4;
        ushr            v4.8h, v4.8h, #8                // vh[0] >>= 8;
        and             v2.16b, v2.16b, v16.16b         // vl[2] &= 0xf;
        and             v1.16b, v1.16b, v16.16b         // vl[1] &= 0xf;
        and             v0.16b, v0.16b, v16.16b         // vl[0] &= 0xf;
        and             v6.16b, v6.16b, v16.16b         // vh[2] &= 0xf;
        and             v5.16b, v5.16b, v16.16b         // vh[1] &= 0xf;
        and             v4.16b, v4.16b, v16.16b         // vh[0] &= 0xf;
        br              x0                              // jump to cont
endfunc

function ff_sws_unpack_0555_8_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // generate masks
        movi            v16.8h, #31
        mov             v2.16b, v0.16b                  // vl[2] = vl[0];
        ushr            v1.8h, v0.8h, #5                // vl[1] >>= 5;
        ushr            v0.8h, v0.8h, #10               // vl[0] >>= 10;
        and             v2.16b, v2.16b, v16.16b         // vl[2] &= 0x1f;
        and             v1.16b, v1.16b, v16.16b         // vl[1] &= 0x1f;
        and             v0.16b, v0.16b, v16.16b         // vl[0] &= 0x1f;
        br              x0                              // jump to cont
endfunc

function ff_sws_unpack_0555_16_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // generate masks
        movi            v16.8h, #31
        mov             v2.16b, v0.16b                  // vl[2] = vl[0];
        ushr            v1.8h, v0.8h, #5                // vl[1] >>= 5;
        ushr            v0.8h, v0.8h, #10               // vl[0] >>= 10;
        mov             v6.16b, v4.16b                  // vh[2] = vh[0];
        ushr            v5.8h, v4.8h, #5                // vh[1] >>= 5;
        ushr            v4.8h, v4.8h, #10               // vh[0] >>= 10;
        and             v2.16b, v2.16b, v16.16b         // vl[2] &= 0x1f;
        and             v1.16b, v1.16b, v16.16b         // vl[1] &= 0x1f;
        and             v0.16b, v0.16b, v16.16b         // vl[0] &= 0x1f;
        and             v6.16b, v6.16b, v16.16b         // vh[2] &= 0x1f;
        and             v5.16b, v5.16b, v16.16b         // vh[1] &= 0x1f;
        and             v4.16b, v4.16b, v16.16b         // vh[0] &= 0x1f;
        br              x0                              // jump to cont
endfunc

function ff_sws_unpack_0565_8_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // generate masks
        movi            v16.8h, #31
        movi            v17.8h, #63
        mov             v2.16b, v0.16b                  // vl[2] = vl[0];
        ushr            v1.8h, v0.8h, #5                // vl[1] >>= 5;
        ushr            v0.8h, v0.8h, #11               // vl[0] >>= 11;
        and             v2.16b, v2.16b, v16.16b         // vl[2] &= 0x1f;
        and             v1.16b, v1.16b, v17.16b         // vl[1] &= 0x3f;
        and             v0.16b, v0.16b, v16.16b         // vl[0] &= 0x1f;
        br              x0                              // jump to cont
endfunc

function ff_sws_unpack_0565_16_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // generate masks
        movi            v16.8h, #31
        movi            v17.8h, #63
        mov             v2.16b, v0.16b                  // vl[2] = vl[0];
        ushr            v1.8h, v0.8h, #5                // vl[1] >>= 5;
        ushr            v0.8h, v0.8h, #11               // vl[0] >>= 11;
        mov             v6.16b, v4.16b                  // vh[2] = vh[0];
        ushr            v5.8h, v4.8h, #5                // vh[1] >>= 5;
        ushr            v4.8h, v4.8h, #11               // vh[0] >>= 11;
        and             v2.16b, v2.16b, v16.16b         // vl[2] &= 0x1f;
        and             v1.16b, v1.16b, v17.16b         // vl[1] &= 0x3f;
        and             v0.16b, v0.16b, v16.16b         // vl[0] &= 0x1f;
        and             v6.16b, v6.16b, v16.16b         // vh[2] &= 0x1f;
        and             v5.16b, v5.16b, v17.16b         // vh[1] &= 0x3f;
        and             v4.16b, v4.16b, v16.16b         // vh[0] &= 0x1f;
        br              x0                              // jump to cont
endfunc

function ff_sws_unpack_2aaa_8_u32_0010_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // generate masks
        mov             w16, #1023
        dup             v16.4s, w16
        ushr            v1.4s, v0.4s, #12               // vl[1] >>= 12;
        ushr            v5.4s, v4.4s, #12               // vh[1] >>= 12;
        and             v1.16b, v1.16b, v16.16b         // vl[1] &= 0x3ff;
        and             v5.16b, v5.16b, v16.16b         // vh[1] &= 0x3ff;
        br              x0                              // jump to cont
endfunc

function ff_sws_unpack_2aaa_8_u32_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // generate masks
        mov             w16, #1023
        dup             v16.4s, w16
        ushr            v2.4s, v0.4s, #2                // vl[2] >>= 2;
        ushr            v1.4s, v0.4s, #12               // vl[1] >>= 12;
        ushr            v0.4s, v0.4s, #22               // vl[0] >>= 22;
        ushr            v6.4s, v4.4s, #2                // vh[2] >>= 2;
        ushr            v5.4s, v4.4s, #12               // vh[1] >>= 12;
        ushr            v4.4s, v4.4s, #22               // vh[0] >>= 22;
        and             v2.16b, v2.16b, v16.16b         // vl[2] &= 0x3ff;
        and             v1.16b, v1.16b, v16.16b         // vl[1] &= 0x3ff;
        and             v0.16b, v0.16b, v16.16b         // vl[0] &= 0x3ff;
        and             v6.16b, v6.16b, v16.16b         // vh[2] &= 0x3ff;
        and             v5.16b, v5.16b, v16.16b         // vh[1] &= 0x3ff;
        and             v4.16b, v4.16b, v16.16b         // vh[0] &= 0x3ff;
        br              x0                              // jump to cont
endfunc

function ff_sws_unpack_aaa2_8_u32_0100_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // generate masks
        mov             w16, #1023
        dup             v16.4s, w16
        ushr            v2.4s, v0.4s, #10               // vl[2] >>= 10;
        ushr            v6.4s, v4.4s, #10               // vh[2] >>= 10;
        and             v2.16b, v2.16b, v16.16b         // vl[2] &= 0x3ff;
        and             v6.16b, v6.16b, v16.16b         // vh[2] &= 0x3ff;
        br              x0                              // jump to cont
endfunc

function ff_sws_unpack_aaa2_8_u32_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // generate masks
        mov             w16, #1023
        dup             v16.4s, w16
        mov             v3.16b, v0.16b                  // vl[3] = vl[0];
        ushr            v2.4s, v0.4s, #10               // vl[2] >>= 10;
        ushr            v1.4s, v0.4s, #20               // vl[1] >>= 20;
        mov             v7.16b, v4.16b                  // vh[3] = vh[0];
        ushr            v6.4s, v4.4s, #10               // vh[2] >>= 10;
        ushr            v5.4s, v4.4s, #20               // vh[1] >>= 20;
        and             v3.16b, v3.16b, v16.16b         // vl[3] &= 0x3ff;
        and             v2.16b, v2.16b, v16.16b         // vl[2] &= 0x3ff;
        and             v1.16b, v1.16b, v16.16b         // vl[1] &= 0x3ff;
        and             v7.16b, v7.16b, v16.16b         // vh[3] &= 0x3ff;
        and             v6.16b, v6.16b, v16.16b         // vh[2] &= 0x3ff;
        and             v5.16b, v5.16b, v16.16b         // vh[1] &= 0x3ff;
        br              x0                              // jump to cont
endfunc

function ff_sws_pack_0121_8_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8b, v0.8b, #3                // vl[0] <<= 3;
        shl             v1.8b, v1.8b, #1                // vl[1] <<= 1;
        orr             v0.16b, v0.16b, v1.16b          // vl[0] |= vl[1];
        orr             v0.16b, v0.16b, v2.16b          // vl[0] |= vl[2];
        br              x0                              // jump to cont
endfunc

function ff_sws_pack_0121_16_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.16b, v0.16b, #3              // vl[0] <<= 3;
        shl             v1.16b, v1.16b, #1              // vl[1] <<= 1;
        orr             v0.16b, v0.16b, v1.16b          // vl[0] |= vl[1];
        orr             v0.16b, v0.16b, v2.16b          // vl[0] |= vl[2];
        br              x0                              // jump to cont
endfunc

function ff_sws_pack_0233_8_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8b, v0.8b, #5                // vl[0] <<= 5;
        shl             v1.8b, v1.8b, #2                // vl[1] <<= 2;
        orr             v0.16b, v0.16b, v1.16b          // vl[0] |= vl[1];
        orr             v0.16b, v0.16b, v2.16b          // vl[0] |= vl[2];
        br              x0                              // jump to cont
endfunc

function ff_sws_pack_0233_16_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.16b, v0.16b, #5              // vl[0] <<= 5;
        shl             v1.16b, v1.16b, #2              // vl[1] <<= 2;
        orr             v0.16b, v0.16b, v1.16b          // vl[0] |= vl[1];
        orr             v0.16b, v0.16b, v2.16b          // vl[0] |= vl[2];
        br              x0                              // jump to cont
endfunc

function ff_sws_pack_0332_8_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8b, v0.8b, #6                // vl[0] <<= 6;
        shl             v1.8b, v1.8b, #3                // vl[1] <<= 3;
        orr             v0.16b, v0.16b, v1.16b          // vl[0] |= vl[1];
        orr             v0.16b, v0.16b, v2.16b          // vl[0] |= vl[2];
        br              x0                              // jump to cont
endfunc

function ff_sws_pack_0332_16_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.16b, v0.16b, #6              // vl[0] <<= 6;
        shl             v1.16b, v1.16b, #3              // vl[1] <<= 3;
        orr             v0.16b, v0.16b, v1.16b          // vl[0] |= vl[1];
        orr             v0.16b, v0.16b, v2.16b          // vl[0] |= vl[2];
        br              x0                              // jump to cont
endfunc

function ff_sws_pack_0444_8_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8h, v0.8h, #8                // vl[0] <<= 8;
        shl             v1.8h, v1.8h, #4                // vl[1] <<= 4;
        orr             v0.16b, v0.16b, v1.16b          // vl[0] |= vl[1];
        orr             v0.16b, v0.16b, v2.16b          // vl[0] |= vl[2];
        br              x0                              // jump to cont
endfunc

function ff_sws_pack_0444_16_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8h, v0.8h, #8                // vl[0] <<= 8;
        shl             v1.8h, v1.8h, #4                // vl[1] <<= 4;
        shl             v4.8h, v4.8h, #8                // vh[0] <<= 8;
        shl             v5.8h, v5.8h, #4                // vh[1] <<= 4;
        orr             v0.16b, v0.16b, v1.16b          // vl[0] |= vl[1];
        orr             v4.16b, v4.16b, v5.16b          // vh[0] |= vh[1];
        orr             v0.16b, v0.16b, v2.16b          // vl[0] |= vl[2];
        orr             v4.16b, v4.16b, v6.16b          // vh[0] |= vh[2];
        br              x0                              // jump to cont
endfunc

function ff_sws_pack_0555_8_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8h, v0.8h, #10               // vl[0] <<= 10;
        shl             v1.8h, v1.8h, #5                // vl[1] <<= 5;
        orr             v0.16b, v0.16b, v1.16b          // vl[0] |= vl[1];
        orr             v0.16b, v0.16b, v2.16b          // vl[0] |= vl[2];
        br              x0                              // jump to cont
endfunc

function ff_sws_pack_0555_16_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8h, v0.8h, #10               // vl[0] <<= 10;
        shl             v1.8h, v1.8h, #5                // vl[1] <<= 5;
        shl             v4.8h, v4.8h, #10               // vh[0] <<= 10;
        shl             v5.8h, v5.8h, #5                // vh[1] <<= 5;
        orr             v0.16b, v0.16b, v1.16b          // vl[0] |= vl[1];
        orr             v4.16b, v4.16b, v5.16b          // vh[0] |= vh[1];
        orr             v0.16b, v0.16b, v2.16b          // vl[0] |= vl[2];
        orr             v4.16b, v4.16b, v6.16b          // vh[0] |= vh[2];
        br              x0                              // jump to cont
endfunc

function ff_sws_pack_0565_8_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8h, v0.8h, #11               // vl[0] <<= 11;
        shl             v1.8h, v1.8h, #5                // vl[1] <<= 5;
        orr             v0.16b, v0.16b, v1.16b          // vl[0] |= vl[1];
        orr             v0.16b, v0.16b, v2.16b          // vl[0] |= vl[2];
        br              x0                              // jump to cont
endfunc

function ff_sws_pack_0565_16_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8h, v0.8h, #11               // vl[0] <<= 11;
        shl             v1.8h, v1.8h, #5                // vl[1] <<= 5;
        shl             v4.8h, v4.8h, #11               // vh[0] <<= 11;
        shl             v5.8h, v5.8h, #5                // vh[1] <<= 5;
        orr             v0.16b, v0.16b, v1.16b          // vl[0] |= vl[1];
        orr             v4.16b, v4.16b, v5.16b          // vh[0] |= vh[1];
        orr             v0.16b, v0.16b, v2.16b          // vl[0] |= vl[2];
        orr             v4.16b, v4.16b, v6.16b          // vh[0] |= vh[2];
        br              x0                              // jump to cont
endfunc

function ff_sws_pack_2aaa_8_u32_1111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.4s, v0.4s, #22               // vl[0] <<= 22;
        shl             v1.4s, v1.4s, #12               // vl[1] <<= 12;
        shl             v2.4s, v2.4s, #2                // vl[2] <<= 2;
        shl             v4.4s, v4.4s, #22               // vh[0] <<= 22;
        shl             v5.4s, v5.4s, #12               // vh[1] <<= 12;
        shl             v6.4s, v6.4s, #2                // vh[2] <<= 2;
        orr             v0.16b, v0.16b, v1.16b          // vl[0] |= vl[1];
        orr             v4.16b, v4.16b, v5.16b          // vh[0] |= vh[1];
        orr             v0.16b, v0.16b, v2.16b          // vl[0] |= vl[2];
        orr             v4.16b, v4.16b, v6.16b          // vh[0] |= vh[2];
        orr             v0.16b, v0.16b, v3.16b          // vl[0] |= vl[3];
        orr             v4.16b, v4.16b, v7.16b          // vh[0] |= vh[3];
        br              x0                              // jump to cont
endfunc

function ff_sws_pack_aaa2_8_u32_1111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.4s, v0.4s, #30               // vl[0] <<= 30;
        shl             v1.4s, v1.4s, #20               // vl[1] <<= 20;
        shl             v2.4s, v2.4s, #10               // vl[2] <<= 10;
        shl             v4.4s, v4.4s, #30               // vh[0] <<= 30;
        shl             v5.4s, v5.4s, #20               // vh[1] <<= 20;
        shl             v6.4s, v6.4s, #10               // vh[2] <<= 10;
        orr             v0.16b, v0.16b, v1.16b          // vl[0] |= vl[1];
        orr             v4.16b, v4.16b, v5.16b          // vh[0] |= vh[1];
        orr             v0.16b, v0.16b, v2.16b          // vl[0] |= vl[2];
        orr             v4.16b, v4.16b, v6.16b          // vh[0] |= vh[2];
        orr             v0.16b, v0.16b, v3.16b          // vl[0] |= vl[3];
        orr             v4.16b, v4.16b, v7.16b          // vh[0] |= vh[3];
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_1_8_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8h, v0.8h, #1                // vl[0] <<= 1;
        shl             v1.8h, v1.8h, #1                // vl[1] <<= 1;
        shl             v2.8h, v2.8h, #1                // vl[2] <<= 1;
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_1_16_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8h, v0.8h, #1                // vl[0] <<= 1;
        shl             v1.8h, v1.8h, #1                // vl[1] <<= 1;
        shl             v2.8h, v2.8h, #1                // vl[2] <<= 1;
        shl             v4.8h, v4.8h, #1                // vh[0] <<= 1;
        shl             v5.8h, v5.8h, #1                // vh[1] <<= 1;
        shl             v6.8h, v6.8h, #1                // vh[2] <<= 1;
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_1_16_u16_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v1.8h, v1.8h, #1                // vl[1] <<= 1;
        shl             v2.8h, v2.8h, #1                // vl[2] <<= 1;
        shl             v3.8h, v3.8h, #1                // vl[3] <<= 1;
        shl             v5.8h, v5.8h, #1                // vh[1] <<= 1;
        shl             v6.8h, v6.8h, #1                // vh[2] <<= 1;
        shl             v7.8h, v7.8h, #1                // vh[3] <<= 1;
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_2_8_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8h, v0.8h, #2                // vl[0] <<= 2;
        shl             v1.8h, v1.8h, #2                // vl[1] <<= 2;
        shl             v2.8h, v2.8h, #2                // vl[2] <<= 2;
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_2_8_u16_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v1.8h, v1.8h, #2                // vl[1] <<= 2;
        shl             v2.8h, v2.8h, #2                // vl[2] <<= 2;
        shl             v3.8h, v3.8h, #2                // vl[3] <<= 2;
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_2_8_u32_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.4s, v0.4s, #2                // vl[0] <<= 2;
        shl             v1.4s, v1.4s, #2                // vl[1] <<= 2;
        shl             v2.4s, v2.4s, #2                // vl[2] <<= 2;
        shl             v4.4s, v4.4s, #2                // vh[0] <<= 2;
        shl             v5.4s, v5.4s, #2                // vh[1] <<= 2;
        shl             v6.4s, v6.4s, #2                // vh[2] <<= 2;
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_2_16_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8h, v0.8h, #2                // vl[0] <<= 2;
        shl             v1.8h, v1.8h, #2                // vl[1] <<= 2;
        shl             v2.8h, v2.8h, #2                // vl[2] <<= 2;
        shl             v4.8h, v4.8h, #2                // vh[0] <<= 2;
        shl             v5.8h, v5.8h, #2                // vh[1] <<= 2;
        shl             v6.8h, v6.8h, #2                // vh[2] <<= 2;
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_2_16_u16_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v1.8h, v1.8h, #2                // vl[1] <<= 2;
        shl             v2.8h, v2.8h, #2                // vl[2] <<= 2;
        shl             v3.8h, v3.8h, #2                // vl[3] <<= 2;
        shl             v5.8h, v5.8h, #2                // vh[1] <<= 2;
        shl             v6.8h, v6.8h, #2                // vh[2] <<= 2;
        shl             v7.8h, v7.8h, #2                // vh[3] <<= 2;
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_3_16_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8h, v0.8h, #3                // vl[0] <<= 3;
        shl             v1.8h, v1.8h, #3                // vl[1] <<= 3;
        shl             v2.8h, v2.8h, #3                // vl[2] <<= 3;
        shl             v4.8h, v4.8h, #3                // vh[0] <<= 3;
        shl             v5.8h, v5.8h, #3                // vh[1] <<= 3;
        shl             v6.8h, v6.8h, #3                // vh[2] <<= 3;
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_4_8_u16_0001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8h, v0.8h, #4                // vl[0] <<= 4;
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_4_8_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8h, v0.8h, #4                // vl[0] <<= 4;
        shl             v1.8h, v1.8h, #4                // vl[1] <<= 4;
        shl             v2.8h, v2.8h, #4                // vl[2] <<= 4;
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_4_8_u16_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v1.8h, v1.8h, #4                // vl[1] <<= 4;
        shl             v2.8h, v2.8h, #4                // vl[2] <<= 4;
        shl             v3.8h, v3.8h, #4                // vl[3] <<= 4;
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_4_16_u16_0001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8h, v0.8h, #4                // vl[0] <<= 4;
        shl             v4.8h, v4.8h, #4                // vh[0] <<= 4;
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_4_16_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8h, v0.8h, #4                // vl[0] <<= 4;
        shl             v1.8h, v1.8h, #4                // vl[1] <<= 4;
        shl             v2.8h, v2.8h, #4                // vl[2] <<= 4;
        shl             v4.8h, v4.8h, #4                // vh[0] <<= 4;
        shl             v5.8h, v5.8h, #4                // vh[1] <<= 4;
        shl             v6.8h, v6.8h, #4                // vh[2] <<= 4;
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_4_16_u16_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v1.8h, v1.8h, #4                // vl[1] <<= 4;
        shl             v2.8h, v2.8h, #4                // vl[2] <<= 4;
        shl             v3.8h, v3.8h, #4                // vl[3] <<= 4;
        shl             v5.8h, v5.8h, #4                // vh[1] <<= 4;
        shl             v6.8h, v6.8h, #4                // vh[2] <<= 4;
        shl             v7.8h, v7.8h, #4                // vh[3] <<= 4;
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_5_16_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8h, v0.8h, #5                // vl[0] <<= 5;
        shl             v1.8h, v1.8h, #5                // vl[1] <<= 5;
        shl             v2.8h, v2.8h, #5                // vl[2] <<= 5;
        shl             v4.8h, v4.8h, #5                // vh[0] <<= 5;
        shl             v5.8h, v5.8h, #5                // vh[1] <<= 5;
        shl             v6.8h, v6.8h, #5                // vh[2] <<= 5;
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_6_8_u16_0001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8h, v0.8h, #6                // vl[0] <<= 6;
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_6_8_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8h, v0.8h, #6                // vl[0] <<= 6;
        shl             v1.8h, v1.8h, #6                // vl[1] <<= 6;
        shl             v2.8h, v2.8h, #6                // vl[2] <<= 6;
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_6_8_u16_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v1.8h, v1.8h, #6                // vl[1] <<= 6;
        shl             v2.8h, v2.8h, #6                // vl[2] <<= 6;
        shl             v3.8h, v3.8h, #6                // vl[3] <<= 6;
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_6_16_u16_0001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8h, v0.8h, #6                // vl[0] <<= 6;
        shl             v4.8h, v4.8h, #6                // vh[0] <<= 6;
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_6_16_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8h, v0.8h, #6                // vl[0] <<= 6;
        shl             v1.8h, v1.8h, #6                // vl[1] <<= 6;
        shl             v2.8h, v2.8h, #6                // vl[2] <<= 6;
        shl             v4.8h, v4.8h, #6                // vh[0] <<= 6;
        shl             v5.8h, v5.8h, #6                // vh[1] <<= 6;
        shl             v6.8h, v6.8h, #6                // vh[2] <<= 6;
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_6_16_u16_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v1.8h, v1.8h, #6                // vl[1] <<= 6;
        shl             v2.8h, v2.8h, #6                // vl[2] <<= 6;
        shl             v3.8h, v3.8h, #6                // vl[3] <<= 6;
        shl             v5.8h, v5.8h, #6                // vh[1] <<= 6;
        shl             v6.8h, v6.8h, #6                // vh[2] <<= 6;
        shl             v7.8h, v7.8h, #6                // vh[3] <<= 6;
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_7_16_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8h, v0.8h, #7                // vl[0] <<= 7;
        shl             v1.8h, v1.8h, #7                // vl[1] <<= 7;
        shl             v2.8h, v2.8h, #7                // vl[2] <<= 7;
        shl             v4.8h, v4.8h, #7                // vh[0] <<= 7;
        shl             v5.8h, v5.8h, #7                // vh[1] <<= 7;
        shl             v6.8h, v6.8h, #7                // vh[2] <<= 7;
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_8_16_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v0.8h, v0.8h, #8                // vl[0] <<= 8;
        shl             v1.8h, v1.8h, #8                // vl[1] <<= 8;
        shl             v2.8h, v2.8h, #8                // vl[2] <<= 8;
        shl             v4.8h, v4.8h, #8                // vh[0] <<= 8;
        shl             v5.8h, v5.8h, #8                // vh[1] <<= 8;
        shl             v6.8h, v6.8h, #8                // vh[2] <<= 8;
        br              x0                              // jump to cont
endfunc

function ff_sws_lshift_8_16_u16_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        shl             v1.8h, v1.8h, #8                // vl[1] <<= 8;
        shl             v2.8h, v2.8h, #8                // vl[2] <<= 8;
        shl             v3.8h, v3.8h, #8                // vl[3] <<= 8;
        shl             v5.8h, v5.8h, #8                // vh[1] <<= 8;
        shl             v6.8h, v6.8h, #8                // vh[2] <<= 8;
        shl             v7.8h, v7.8h, #8                // vh[3] <<= 8;
        br              x0                              // jump to cont
endfunc

function ff_sws_rshift_4_8_u16_0001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ushr            v0.8h, v0.8h, #4                // vl[0] >>= 4;
        br              x0                              // jump to cont
endfunc

function ff_sws_rshift_4_8_u16_0010_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ushr            v1.8h, v1.8h, #4                // vl[1] >>= 4;
        br              x0                              // jump to cont
endfunc

function ff_sws_rshift_4_8_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ushr            v0.8h, v0.8h, #4                // vl[0] >>= 4;
        ushr            v1.8h, v1.8h, #4                // vl[1] >>= 4;
        ushr            v2.8h, v2.8h, #4                // vl[2] >>= 4;
        br              x0                              // jump to cont
endfunc

function ff_sws_rshift_4_16_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ushr            v0.8h, v0.8h, #4                // vl[0] >>= 4;
        ushr            v1.8h, v1.8h, #4                // vl[1] >>= 4;
        ushr            v2.8h, v2.8h, #4                // vl[2] >>= 4;
        ushr            v4.8h, v4.8h, #4                // vh[0] >>= 4;
        ushr            v5.8h, v5.8h, #4                // vh[1] >>= 4;
        ushr            v6.8h, v6.8h, #4                // vh[2] >>= 4;
        br              x0                              // jump to cont
endfunc

function ff_sws_rshift_6_8_u16_0001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ushr            v0.8h, v0.8h, #6                // vl[0] >>= 6;
        br              x0                              // jump to cont
endfunc

function ff_sws_rshift_6_8_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ushr            v0.8h, v0.8h, #6                // vl[0] >>= 6;
        ushr            v1.8h, v1.8h, #6                // vl[1] >>= 6;
        ushr            v2.8h, v2.8h, #6                // vl[2] >>= 6;
        br              x0                              // jump to cont
endfunc

function ff_sws_rshift_6_16_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ushr            v0.8h, v0.8h, #6                // vl[0] >>= 6;
        ushr            v1.8h, v1.8h, #6                // vl[1] >>= 6;
        ushr            v2.8h, v2.8h, #6                // vl[2] >>= 6;
        ushr            v4.8h, v4.8h, #6                // vh[0] >>= 6;
        ushr            v5.8h, v5.8h, #6                // vh[1] >>= 6;
        ushr            v6.8h, v6.8h, #6                // vh[2] >>= 6;
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_8_u8_0001_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v0.8b, v16.b[0]                 // vl[0] = broadcast(clear_vec[0])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_8_u8_0010_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v1.8b, v16.b[1]                 // vl[1] = broadcast(clear_vec[1])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_8_u8_0011_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v0.8b, v16.b[0]                 // vl[0] = broadcast(clear_vec[0])
        dup             v1.8b, v16.b[1]                 // vl[1] = broadcast(clear_vec[1])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_8_u8_0101_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v0.8b, v16.b[0]                 // vl[0] = broadcast(clear_vec[0])
        dup             v2.8b, v16.b[2]                 // vl[2] = broadcast(clear_vec[2])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_8_u8_0110_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v1.8b, v16.b[1]                 // vl[1] = broadcast(clear_vec[1])
        dup             v2.8b, v16.b[2]                 // vl[2] = broadcast(clear_vec[2])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_8_u8_1000_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v3.8b, v16.b[3]                 // vl[3] = broadcast(clear_vec[3])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_8_u8_1011_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v0.8b, v16.b[0]                 // vl[0] = broadcast(clear_vec[0])
        dup             v1.8b, v16.b[1]                 // vl[1] = broadcast(clear_vec[1])
        dup             v3.8b, v16.b[3]                 // vl[3] = broadcast(clear_vec[3])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_8_u8_1100_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v2.8b, v16.b[2]                 // vl[2] = broadcast(clear_vec[2])
        dup             v3.8b, v16.b[3]                 // vl[3] = broadcast(clear_vec[3])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_8_u8_1101_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v0.8b, v16.b[0]                 // vl[0] = broadcast(clear_vec[0])
        dup             v2.8b, v16.b[2]                 // vl[2] = broadcast(clear_vec[2])
        dup             v3.8b, v16.b[3]                 // vl[3] = broadcast(clear_vec[3])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_8_u8_1110_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v1.8b, v16.b[1]                 // vl[1] = broadcast(clear_vec[1])
        dup             v2.8b, v16.b[2]                 // vl[2] = broadcast(clear_vec[2])
        dup             v3.8b, v16.b[3]                 // vl[3] = broadcast(clear_vec[3])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_8_u16_0001_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v0.8h, v16.h[0]                 // vl[0] = broadcast(clear_vec[0])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_8_u16_0010_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v1.8h, v16.h[1]                 // vl[1] = broadcast(clear_vec[1])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_8_u16_0110_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v1.8h, v16.h[1]                 // vl[1] = broadcast(clear_vec[1])
        dup             v2.8h, v16.h[2]                 // vl[2] = broadcast(clear_vec[2])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_8_u16_1000_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v3.8h, v16.h[3]                 // vl[3] = broadcast(clear_vec[3])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_8_u16_1100_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v2.8h, v16.h[2]                 // vl[2] = broadcast(clear_vec[2])
        dup             v3.8h, v16.h[3]                 // vl[3] = broadcast(clear_vec[3])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_8_u16_1101_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v0.8h, v16.h[0]                 // vl[0] = broadcast(clear_vec[0])
        dup             v2.8h, v16.h[2]                 // vl[2] = broadcast(clear_vec[2])
        dup             v3.8h, v16.h[3]                 // vl[3] = broadcast(clear_vec[3])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_8_u16_1110_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v1.8h, v16.h[1]                 // vl[1] = broadcast(clear_vec[1])
        dup             v2.8h, v16.h[2]                 // vl[2] = broadcast(clear_vec[2])
        dup             v3.8h, v16.h[3]                 // vl[3] = broadcast(clear_vec[3])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_8_u32_0001_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v0.4s, v16.s[0]                 // vl[0] = broadcast(clear_vec[0])
        dup             v4.4s, v16.s[0]                 // vh[0] = broadcast(clear_vec[0])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_8_u32_0010_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v1.4s, v16.s[1]                 // vl[1] = broadcast(clear_vec[1])
        dup             v5.4s, v16.s[1]                 // vh[1] = broadcast(clear_vec[1])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_8_u32_0101_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v0.4s, v16.s[0]                 // vl[0] = broadcast(clear_vec[0])
        dup             v2.4s, v16.s[2]                 // vl[2] = broadcast(clear_vec[2])
        dup             v4.4s, v16.s[0]                 // vh[0] = broadcast(clear_vec[0])
        dup             v6.4s, v16.s[2]                 // vh[2] = broadcast(clear_vec[2])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_8_u32_1000_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v3.4s, v16.s[3]                 // vl[3] = broadcast(clear_vec[3])
        dup             v7.4s, v16.s[3]                 // vh[3] = broadcast(clear_vec[3])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_8_u32_1010_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v1.4s, v16.s[1]                 // vl[1] = broadcast(clear_vec[1])
        dup             v3.4s, v16.s[3]                 // vl[3] = broadcast(clear_vec[3])
        dup             v5.4s, v16.s[1]                 // vh[1] = broadcast(clear_vec[1])
        dup             v7.4s, v16.s[3]                 // vh[3] = broadcast(clear_vec[3])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_8_u32_1011_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v0.4s, v16.s[0]                 // vl[0] = broadcast(clear_vec[0])
        dup             v1.4s, v16.s[1]                 // vl[1] = broadcast(clear_vec[1])
        dup             v3.4s, v16.s[3]                 // vl[3] = broadcast(clear_vec[3])
        dup             v4.4s, v16.s[0]                 // vh[0] = broadcast(clear_vec[0])
        dup             v5.4s, v16.s[1]                 // vh[1] = broadcast(clear_vec[1])
        dup             v7.4s, v16.s[3]                 // vh[3] = broadcast(clear_vec[3])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_8_u32_1101_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v0.4s, v16.s[0]                 // vl[0] = broadcast(clear_vec[0])
        dup             v2.4s, v16.s[2]                 // vl[2] = broadcast(clear_vec[2])
        dup             v3.4s, v16.s[3]                 // vl[3] = broadcast(clear_vec[3])
        dup             v4.4s, v16.s[0]                 // vh[0] = broadcast(clear_vec[0])
        dup             v6.4s, v16.s[2]                 // vh[2] = broadcast(clear_vec[2])
        dup             v7.4s, v16.s[3]                 // vh[3] = broadcast(clear_vec[3])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_16_u8_0001_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v0.16b, v16.b[0]                // vl[0] = broadcast(clear_vec[0])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_16_u8_0010_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v1.16b, v16.b[1]                // vl[1] = broadcast(clear_vec[1])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_16_u8_0110_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v1.16b, v16.b[1]                // vl[1] = broadcast(clear_vec[1])
        dup             v2.16b, v16.b[2]                // vl[2] = broadcast(clear_vec[2])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_16_u8_1000_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v3.16b, v16.b[3]                // vl[3] = broadcast(clear_vec[3])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_16_u16_0001_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v0.8h, v16.h[0]                 // vl[0] = broadcast(clear_vec[0])
        dup             v4.8h, v16.h[0]                 // vh[0] = broadcast(clear_vec[0])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_16_u16_0010_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v1.8h, v16.h[1]                 // vl[1] = broadcast(clear_vec[1])
        dup             v5.8h, v16.h[1]                 // vh[1] = broadcast(clear_vec[1])
        br              x0                              // jump to cont
endfunc

function ff_sws_clear_16_u16_1000_neon, export=1, jumpable=1
        ldr             q16, [x1, #16]                  // v128 clear_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v3.8h, v16.h[3]                 // vl[3] = broadcast(clear_vec[3])
        dup             v7.8h, v16.h[3]                 // vh[3] = broadcast(clear_vec[3])
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u8_8_f32_0001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // f32 -> u32
        fcvtzu          v0.4s, v0.4s
        fcvtzu          v4.4s, v4.4s
        // u32 -> u16
        xtn             v0.4h, v0.4s
        xtn             v4.4h, v4.4s
        ins             v0.d[1], v4.d[0]
        // u16 -> u8
        xtn             v0.8b, v0.8h
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u8_8_f32_0011_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // f32 -> u32
        fcvtzu          v0.4s, v0.4s
        fcvtzu          v1.4s, v1.4s
        fcvtzu          v4.4s, v4.4s
        fcvtzu          v5.4s, v5.4s
        // u32 -> u16
        xtn             v0.4h, v0.4s
        xtn             v1.4h, v1.4s
        xtn             v4.4h, v4.4s
        xtn             v5.4h, v5.4s
        ins             v0.d[1], v4.d[0]
        ins             v1.d[1], v5.d[0]
        // u16 -> u8
        xtn             v0.8b, v0.8h
        xtn             v1.8b, v1.8h
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u8_8_f32_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // f32 -> u32
        fcvtzu          v0.4s, v0.4s
        fcvtzu          v1.4s, v1.4s
        fcvtzu          v2.4s, v2.4s
        fcvtzu          v4.4s, v4.4s
        fcvtzu          v5.4s, v5.4s
        fcvtzu          v6.4s, v6.4s
        // u32 -> u16
        xtn             v0.4h, v0.4s
        xtn             v1.4h, v1.4s
        xtn             v2.4h, v2.4s
        xtn             v4.4h, v4.4s
        xtn             v5.4h, v5.4s
        xtn             v6.4h, v6.4s
        ins             v0.d[1], v4.d[0]
        ins             v1.d[1], v5.d[0]
        ins             v2.d[1], v6.d[0]
        // u16 -> u8
        xtn             v0.8b, v0.8h
        xtn             v1.8b, v1.8h
        xtn             v2.8b, v2.8h
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u8_8_f32_1001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // f32 -> u32
        fcvtzu          v0.4s, v0.4s
        fcvtzu          v3.4s, v3.4s
        fcvtzu          v4.4s, v4.4s
        fcvtzu          v7.4s, v7.4s
        // u32 -> u16
        xtn             v0.4h, v0.4s
        xtn             v3.4h, v3.4s
        xtn             v4.4h, v4.4s
        xtn             v7.4h, v7.4s
        ins             v0.d[1], v4.d[0]
        ins             v3.d[1], v7.d[0]
        // u16 -> u8
        xtn             v0.8b, v0.8h
        xtn             v3.8b, v3.8h
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u8_8_f32_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // f32 -> u32
        fcvtzu          v1.4s, v1.4s
        fcvtzu          v2.4s, v2.4s
        fcvtzu          v3.4s, v3.4s
        fcvtzu          v5.4s, v5.4s
        fcvtzu          v6.4s, v6.4s
        fcvtzu          v7.4s, v7.4s
        // u32 -> u16
        xtn             v1.4h, v1.4s
        xtn             v2.4h, v2.4s
        xtn             v3.4h, v3.4s
        xtn             v5.4h, v5.4s
        xtn             v6.4h, v6.4s
        xtn             v7.4h, v7.4s
        ins             v1.d[1], v5.d[0]
        ins             v2.d[1], v6.d[0]
        ins             v3.d[1], v7.d[0]
        // u16 -> u8
        xtn             v1.8b, v1.8h
        xtn             v2.8b, v2.8h
        xtn             v3.8b, v3.8h
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u8_8_f32_1111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // f32 -> u32
        fcvtzu          v0.4s, v0.4s
        fcvtzu          v1.4s, v1.4s
        fcvtzu          v2.4s, v2.4s
        fcvtzu          v3.4s, v3.4s
        fcvtzu          v4.4s, v4.4s
        fcvtzu          v5.4s, v5.4s
        fcvtzu          v6.4s, v6.4s
        fcvtzu          v7.4s, v7.4s
        // u32 -> u16
        xtn             v0.4h, v0.4s
        xtn             v1.4h, v1.4s
        xtn             v2.4h, v2.4s
        xtn             v3.4h, v3.4s
        xtn             v4.4h, v4.4s
        xtn             v5.4h, v5.4s
        xtn             v6.4h, v6.4s
        xtn             v7.4h, v7.4s
        ins             v0.d[1], v4.d[0]
        ins             v1.d[1], v5.d[0]
        ins             v2.d[1], v6.d[0]
        ins             v3.d[1], v7.d[0]
        // u16 -> u8
        xtn             v0.8b, v0.8h
        xtn             v1.8b, v1.8h
        xtn             v2.8b, v2.8h
        xtn             v3.8b, v3.8h
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u8_16_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u16 -> u8
        xtn             v0.8b, v0.8h
        xtn             v1.8b, v1.8h
        xtn             v2.8b, v2.8h
        xtn             v4.8b, v4.8h
        xtn             v5.8b, v5.8h
        xtn             v6.8b, v6.8h
        ins             v0.d[1], v4.d[0]
        ins             v1.d[1], v5.d[0]
        ins             v2.d[1], v6.d[0]
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u16_8_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u8 -> u16
        uxtl            v0.8h, v0.8b
        uxtl            v1.8h, v1.8b
        uxtl            v2.8h, v2.8b
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u16_8_u32_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u32 -> u16
        xtn             v0.4h, v0.4s
        xtn             v1.4h, v1.4s
        xtn             v2.4h, v2.4s
        xtn             v4.4h, v4.4s
        xtn             v5.4h, v5.4s
        xtn             v6.4h, v6.4s
        ins             v0.d[1], v4.d[0]
        ins             v1.d[1], v5.d[0]
        ins             v2.d[1], v6.d[0]
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u16_8_u32_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u32 -> u16
        xtn             v1.4h, v1.4s
        xtn             v2.4h, v2.4s
        xtn             v3.4h, v3.4s
        xtn             v5.4h, v5.4s
        xtn             v6.4h, v6.4s
        xtn             v7.4h, v7.4s
        ins             v1.d[1], v5.d[0]
        ins             v2.d[1], v6.d[0]
        ins             v3.d[1], v7.d[0]
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u16_8_f32_0001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // f32 -> u32
        fcvtzu          v0.4s, v0.4s
        fcvtzu          v4.4s, v4.4s
        // u32 -> u16
        xtn             v0.4h, v0.4s
        xtn             v4.4h, v4.4s
        ins             v0.d[1], v4.d[0]
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u16_8_f32_0011_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // f32 -> u32
        fcvtzu          v0.4s, v0.4s
        fcvtzu          v1.4s, v1.4s
        fcvtzu          v4.4s, v4.4s
        fcvtzu          v5.4s, v5.4s
        // u32 -> u16
        xtn             v0.4h, v0.4s
        xtn             v1.4h, v1.4s
        xtn             v4.4h, v4.4s
        xtn             v5.4h, v5.4s
        ins             v0.d[1], v4.d[0]
        ins             v1.d[1], v5.d[0]
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u16_8_f32_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // f32 -> u32
        fcvtzu          v0.4s, v0.4s
        fcvtzu          v1.4s, v1.4s
        fcvtzu          v2.4s, v2.4s
        fcvtzu          v4.4s, v4.4s
        fcvtzu          v5.4s, v5.4s
        fcvtzu          v6.4s, v6.4s
        // u32 -> u16
        xtn             v0.4h, v0.4s
        xtn             v1.4h, v1.4s
        xtn             v2.4h, v2.4s
        xtn             v4.4h, v4.4s
        xtn             v5.4h, v5.4s
        xtn             v6.4h, v6.4s
        ins             v0.d[1], v4.d[0]
        ins             v1.d[1], v5.d[0]
        ins             v2.d[1], v6.d[0]
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u16_8_f32_1001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // f32 -> u32
        fcvtzu          v0.4s, v0.4s
        fcvtzu          v3.4s, v3.4s
        fcvtzu          v4.4s, v4.4s
        fcvtzu          v7.4s, v7.4s
        // u32 -> u16
        xtn             v0.4h, v0.4s
        xtn             v3.4h, v3.4s
        xtn             v4.4h, v4.4s
        xtn             v7.4h, v7.4s
        ins             v0.d[1], v4.d[0]
        ins             v3.d[1], v7.d[0]
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u16_8_f32_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // f32 -> u32
        fcvtzu          v1.4s, v1.4s
        fcvtzu          v2.4s, v2.4s
        fcvtzu          v3.4s, v3.4s
        fcvtzu          v5.4s, v5.4s
        fcvtzu          v6.4s, v6.4s
        fcvtzu          v7.4s, v7.4s
        // u32 -> u16
        xtn             v1.4h, v1.4s
        xtn             v2.4h, v2.4s
        xtn             v3.4h, v3.4s
        xtn             v5.4h, v5.4s
        xtn             v6.4h, v6.4s
        xtn             v7.4h, v7.4s
        ins             v1.d[1], v5.d[0]
        ins             v2.d[1], v6.d[0]
        ins             v3.d[1], v7.d[0]
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u16_8_f32_1111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // f32 -> u32
        fcvtzu          v0.4s, v0.4s
        fcvtzu          v1.4s, v1.4s
        fcvtzu          v2.4s, v2.4s
        fcvtzu          v3.4s, v3.4s
        fcvtzu          v4.4s, v4.4s
        fcvtzu          v5.4s, v5.4s
        fcvtzu          v6.4s, v6.4s
        fcvtzu          v7.4s, v7.4s
        // u32 -> u16
        xtn             v0.4h, v0.4s
        xtn             v1.4h, v1.4s
        xtn             v2.4h, v2.4s
        xtn             v3.4h, v3.4s
        xtn             v4.4h, v4.4s
        xtn             v5.4h, v5.4s
        xtn             v6.4h, v6.4s
        xtn             v7.4h, v7.4s
        ins             v0.d[1], v4.d[0]
        ins             v1.d[1], v5.d[0]
        ins             v2.d[1], v6.d[0]
        ins             v3.d[1], v7.d[0]
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u16_16_u8_0001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u8 -> u16
        uxtl2           v4.8h, v0.16b
        uxtl            v0.8h, v0.8b
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u16_16_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u8 -> u16
        uxtl2           v4.8h, v0.16b
        uxtl2           v5.8h, v1.16b
        uxtl2           v6.8h, v2.16b
        uxtl            v0.8h, v0.8b
        uxtl            v1.8h, v1.8b
        uxtl            v2.8h, v2.8b
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u16_16_u8_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u8 -> u16
        uxtl2           v5.8h, v1.16b
        uxtl2           v6.8h, v2.16b
        uxtl2           v7.8h, v3.16b
        uxtl            v1.8h, v1.8b
        uxtl            v2.8h, v2.8b
        uxtl            v3.8h, v3.8b
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u32_8_u8_0001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u8 -> u16
        uxtl            v0.8h, v0.8b
        // u16 -> u32
        uxtl2           v4.4s, v0.8h
        uxtl            v0.4s, v0.4h
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u32_8_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u8 -> u16
        uxtl            v0.8h, v0.8b
        uxtl            v1.8h, v1.8b
        uxtl            v2.8h, v2.8b
        // u16 -> u32
        uxtl2           v4.4s, v0.8h
        uxtl2           v5.4s, v1.8h
        uxtl2           v6.4s, v2.8h
        uxtl            v0.4s, v0.4h
        uxtl            v1.4s, v1.4h
        uxtl            v2.4s, v2.4h
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u32_8_u16_0001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u16 -> u32
        uxtl2           v4.4s, v0.8h
        uxtl            v0.4s, v0.4h
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u32_8_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u16 -> u32
        uxtl2           v4.4s, v0.8h
        uxtl2           v5.4s, v1.8h
        uxtl2           v6.4s, v2.8h
        uxtl            v0.4s, v0.4h
        uxtl            v1.4s, v1.4h
        uxtl            v2.4s, v2.4h
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u32_8_f32_0001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // f32 -> u32
        fcvtzu          v0.4s, v0.4s
        fcvtzu          v4.4s, v4.4s
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u32_8_f32_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // f32 -> u32
        fcvtzu          v0.4s, v0.4s
        fcvtzu          v1.4s, v1.4s
        fcvtzu          v2.4s, v2.4s
        fcvtzu          v4.4s, v4.4s
        fcvtzu          v5.4s, v5.4s
        fcvtzu          v6.4s, v6.4s
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u32_8_f32_1001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // f32 -> u32
        fcvtzu          v0.4s, v0.4s
        fcvtzu          v3.4s, v3.4s
        fcvtzu          v4.4s, v4.4s
        fcvtzu          v7.4s, v7.4s
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u32_8_f32_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // f32 -> u32
        fcvtzu          v1.4s, v1.4s
        fcvtzu          v2.4s, v2.4s
        fcvtzu          v3.4s, v3.4s
        fcvtzu          v5.4s, v5.4s
        fcvtzu          v6.4s, v6.4s
        fcvtzu          v7.4s, v7.4s
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_u32_8_f32_1111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // f32 -> u32
        fcvtzu          v0.4s, v0.4s
        fcvtzu          v1.4s, v1.4s
        fcvtzu          v2.4s, v2.4s
        fcvtzu          v3.4s, v3.4s
        fcvtzu          v4.4s, v4.4s
        fcvtzu          v5.4s, v5.4s
        fcvtzu          v6.4s, v6.4s
        fcvtzu          v7.4s, v7.4s
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_f32_8_u8_0001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u8 -> u16
        uxtl            v0.8h, v0.8b
        // u16 -> u32
        uxtl2           v4.4s, v0.8h
        uxtl            v0.4s, v0.4h
        // u32 -> f32
        ucvtf           v0.4s, v0.4s
        ucvtf           v4.4s, v4.4s
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_f32_8_u8_0010_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u8 -> u16
        uxtl            v1.8h, v1.8b
        // u16 -> u32
        uxtl2           v5.4s, v1.8h
        uxtl            v1.4s, v1.4h
        // u32 -> f32
        ucvtf           v1.4s, v1.4s
        ucvtf           v5.4s, v5.4s
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_f32_8_u8_0011_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u8 -> u16
        uxtl            v0.8h, v0.8b
        uxtl            v1.8h, v1.8b
        // u16 -> u32
        uxtl2           v4.4s, v0.8h
        uxtl2           v5.4s, v1.8h
        uxtl            v0.4s, v0.4h
        uxtl            v1.4s, v1.4h
        // u32 -> f32
        ucvtf           v0.4s, v0.4s
        ucvtf           v1.4s, v1.4s
        ucvtf           v4.4s, v4.4s
        ucvtf           v5.4s, v5.4s
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_f32_8_u8_0100_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u8 -> u16
        uxtl            v2.8h, v2.8b
        // u16 -> u32
        uxtl2           v6.4s, v2.8h
        uxtl            v2.4s, v2.4h
        // u32 -> f32
        ucvtf           v2.4s, v2.4s
        ucvtf           v6.4s, v6.4s
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_f32_8_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u8 -> u16
        uxtl            v0.8h, v0.8b
        uxtl            v1.8h, v1.8b
        uxtl            v2.8h, v2.8b
        // u16 -> u32
        uxtl2           v4.4s, v0.8h
        uxtl2           v5.4s, v1.8h
        uxtl2           v6.4s, v2.8h
        uxtl            v0.4s, v0.4h
        uxtl            v1.4s, v1.4h
        uxtl            v2.4s, v2.4h
        // u32 -> f32
        ucvtf           v0.4s, v0.4s
        ucvtf           v1.4s, v1.4s
        ucvtf           v2.4s, v2.4s
        ucvtf           v4.4s, v4.4s
        ucvtf           v5.4s, v5.4s
        ucvtf           v6.4s, v6.4s
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_f32_8_u8_1010_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u8 -> u16
        uxtl            v1.8h, v1.8b
        uxtl            v3.8h, v3.8b
        // u16 -> u32
        uxtl2           v5.4s, v1.8h
        uxtl2           v7.4s, v3.8h
        uxtl            v1.4s, v1.4h
        uxtl            v3.4s, v3.4h
        // u32 -> f32
        ucvtf           v1.4s, v1.4s
        ucvtf           v3.4s, v3.4s
        ucvtf           v5.4s, v5.4s
        ucvtf           v7.4s, v7.4s
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_f32_8_u8_1100_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u8 -> u16
        uxtl            v2.8h, v2.8b
        uxtl            v3.8h, v3.8b
        // u16 -> u32
        uxtl2           v6.4s, v2.8h
        uxtl2           v7.4s, v3.8h
        uxtl            v2.4s, v2.4h
        uxtl            v3.4s, v3.4h
        // u32 -> f32
        ucvtf           v2.4s, v2.4s
        ucvtf           v3.4s, v3.4s
        ucvtf           v6.4s, v6.4s
        ucvtf           v7.4s, v7.4s
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_f32_8_u8_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u8 -> u16
        uxtl            v1.8h, v1.8b
        uxtl            v2.8h, v2.8b
        uxtl            v3.8h, v3.8b
        // u16 -> u32
        uxtl2           v5.4s, v1.8h
        uxtl2           v6.4s, v2.8h
        uxtl2           v7.4s, v3.8h
        uxtl            v1.4s, v1.4h
        uxtl            v2.4s, v2.4h
        uxtl            v3.4s, v3.4h
        // u32 -> f32
        ucvtf           v1.4s, v1.4s
        ucvtf           v2.4s, v2.4s
        ucvtf           v3.4s, v3.4s
        ucvtf           v5.4s, v5.4s
        ucvtf           v6.4s, v6.4s
        ucvtf           v7.4s, v7.4s
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_f32_8_u8_1111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u8 -> u16
        uxtl            v0.8h, v0.8b
        uxtl            v1.8h, v1.8b
        uxtl            v2.8h, v2.8b
        uxtl            v3.8h, v3.8b
        // u16 -> u32
        uxtl2           v4.4s, v0.8h
        uxtl2           v5.4s, v1.8h
        uxtl2           v6.4s, v2.8h
        uxtl2           v7.4s, v3.8h
        uxtl            v0.4s, v0.4h
        uxtl            v1.4s, v1.4h
        uxtl            v2.4s, v2.4h
        uxtl            v3.4s, v3.4h
        // u32 -> f32
        ucvtf           v0.4s, v0.4s
        ucvtf           v1.4s, v1.4s
        ucvtf           v2.4s, v2.4s
        ucvtf           v3.4s, v3.4s
        ucvtf           v4.4s, v4.4s
        ucvtf           v5.4s, v5.4s
        ucvtf           v6.4s, v6.4s
        ucvtf           v7.4s, v7.4s
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_f32_8_u16_0001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u16 -> u32
        uxtl2           v4.4s, v0.8h
        uxtl            v0.4s, v0.4h
        // u32 -> f32
        ucvtf           v0.4s, v0.4s
        ucvtf           v4.4s, v4.4s
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_f32_8_u16_0010_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u16 -> u32
        uxtl2           v5.4s, v1.8h
        uxtl            v1.4s, v1.4h
        // u32 -> f32
        ucvtf           v1.4s, v1.4s
        ucvtf           v5.4s, v5.4s
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_f32_8_u16_0011_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u16 -> u32
        uxtl2           v4.4s, v0.8h
        uxtl2           v5.4s, v1.8h
        uxtl            v0.4s, v0.4h
        uxtl            v1.4s, v1.4h
        // u32 -> f32
        ucvtf           v0.4s, v0.4s
        ucvtf           v1.4s, v1.4s
        ucvtf           v4.4s, v4.4s
        ucvtf           v5.4s, v5.4s
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_f32_8_u16_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u16 -> u32
        uxtl2           v4.4s, v0.8h
        uxtl2           v5.4s, v1.8h
        uxtl2           v6.4s, v2.8h
        uxtl            v0.4s, v0.4h
        uxtl            v1.4s, v1.4h
        uxtl            v2.4s, v2.4h
        // u32 -> f32
        ucvtf           v0.4s, v0.4s
        ucvtf           v1.4s, v1.4s
        ucvtf           v2.4s, v2.4s
        ucvtf           v4.4s, v4.4s
        ucvtf           v5.4s, v5.4s
        ucvtf           v6.4s, v6.4s
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_f32_8_u16_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u16 -> u32
        uxtl2           v5.4s, v1.8h
        uxtl2           v6.4s, v2.8h
        uxtl2           v7.4s, v3.8h
        uxtl            v1.4s, v1.4h
        uxtl            v2.4s, v2.4h
        uxtl            v3.4s, v3.4h
        // u32 -> f32
        ucvtf           v1.4s, v1.4s
        ucvtf           v2.4s, v2.4s
        ucvtf           v3.4s, v3.4s
        ucvtf           v5.4s, v5.4s
        ucvtf           v6.4s, v6.4s
        ucvtf           v7.4s, v7.4s
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_f32_8_u16_1111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u16 -> u32
        uxtl2           v4.4s, v0.8h
        uxtl2           v5.4s, v1.8h
        uxtl2           v6.4s, v2.8h
        uxtl2           v7.4s, v3.8h
        uxtl            v0.4s, v0.4h
        uxtl            v1.4s, v1.4h
        uxtl            v2.4s, v2.4h
        uxtl            v3.4s, v3.4h
        // u32 -> f32
        ucvtf           v0.4s, v0.4s
        ucvtf           v1.4s, v1.4s
        ucvtf           v2.4s, v2.4s
        ucvtf           v3.4s, v3.4s
        ucvtf           v4.4s, v4.4s
        ucvtf           v5.4s, v5.4s
        ucvtf           v6.4s, v6.4s
        ucvtf           v7.4s, v7.4s
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_f32_8_u32_0010_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u32 -> f32
        ucvtf           v1.4s, v1.4s
        ucvtf           v5.4s, v5.4s
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_f32_8_u32_0100_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u32 -> f32
        ucvtf           v2.4s, v2.4s
        ucvtf           v6.4s, v6.4s
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_f32_8_u32_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u32 -> f32
        ucvtf           v0.4s, v0.4s
        ucvtf           v1.4s, v1.4s
        ucvtf           v2.4s, v2.4s
        ucvtf           v4.4s, v4.4s
        ucvtf           v5.4s, v5.4s
        ucvtf           v6.4s, v6.4s
        br              x0                              // jump to cont
endfunc

function ff_sws_convert_f32_8_u32_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u32 -> f32
        ucvtf           v1.4s, v1.4s
        ucvtf           v2.4s, v2.4s
        ucvtf           v3.4s, v3.4s
        ucvtf           v5.4s, v5.4s
        ucvtf           v6.4s, v6.4s
        ucvtf           v7.4s, v7.4s
        br              x0                              // jump to cont
endfunc

function ff_sws_expand_u16_16_u8_0001_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u8 -> u16
        zip2            v4.16b, v0.16b, v0.16b
        zip1            v0.16b, v0.16b, v0.16b
        br              x0                              // jump to cont
endfunc

function ff_sws_expand_u16_16_u8_0011_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u8 -> u16
        zip2            v4.16b, v0.16b, v0.16b
        zip2            v5.16b, v1.16b, v1.16b
        zip1            v0.16b, v0.16b, v0.16b
        zip1            v1.16b, v1.16b, v1.16b
        br              x0                              // jump to cont
endfunc

function ff_sws_expand_u16_16_u8_0111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u8 -> u16
        zip2            v4.16b, v0.16b, v0.16b
        zip2            v5.16b, v1.16b, v1.16b
        zip2            v6.16b, v2.16b, v2.16b
        zip1            v0.16b, v0.16b, v0.16b
        zip1            v1.16b, v1.16b, v1.16b
        zip1            v2.16b, v2.16b, v2.16b
        br              x0                              // jump to cont
endfunc

function ff_sws_expand_u16_16_u8_1110_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u8 -> u16
        zip2            v5.16b, v1.16b, v1.16b
        zip2            v6.16b, v2.16b, v2.16b
        zip2            v7.16b, v3.16b, v3.16b
        zip1            v1.16b, v1.16b, v1.16b
        zip1            v2.16b, v2.16b, v2.16b
        zip1            v3.16b, v3.16b, v3.16b
        br              x0                              // jump to cont
endfunc

function ff_sws_expand_u16_16_u8_1111_neon, export=1, jumpable=1
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        // u8 -> u16
        zip2            v4.16b, v0.16b, v0.16b
        zip2            v5.16b, v1.16b, v1.16b
        zip2            v6.16b, v2.16b, v2.16b
        zip2            v7.16b, v3.16b, v3.16b
        zip1            v0.16b, v0.16b, v0.16b
        zip1            v1.16b, v1.16b, v1.16b
        zip1            v2.16b, v2.16b, v2.16b
        zip1            v3.16b, v3.16b, v3.16b
        br              x0                              // jump to cont
endfunc

function ff_sws_min_8_f32_0001_neon, export=1, jumpable=1
        ldr             q20, [x1, #16]                  // v128 min_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v16.4s, v20.s[0]                // v128 vmin0 = min_vec[0];
        fmin            v0.4s, v0.4s, v16.4s            // vl[0] = min(vl[0], vmin0);
        fmin            v4.4s, v4.4s, v16.4s            // vh[0] = min(vh[0], vmin0);
        br              x0                              // jump to cont
endfunc

function ff_sws_min_8_f32_0011_neon, export=1, jumpable=1
        ldr             q20, [x1, #16]                  // v128 min_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v16.4s, v20.s[0]                // v128 vmin0 = min_vec[0];
        dup             v17.4s, v20.s[1]                // v128 vmin1 = min_vec[1];
        fmin            v0.4s, v0.4s, v16.4s            // vl[0] = min(vl[0], vmin0);
        fmin            v1.4s, v1.4s, v17.4s            // vl[1] = min(vl[1], vmin1);
        fmin            v4.4s, v4.4s, v16.4s            // vh[0] = min(vh[0], vmin0);
        fmin            v5.4s, v5.4s, v17.4s            // vh[1] = min(vh[1], vmin1);
        br              x0                              // jump to cont
endfunc

function ff_sws_min_8_f32_0111_neon, export=1, jumpable=1
        ldr             q20, [x1, #16]                  // v128 min_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v16.4s, v20.s[0]                // v128 vmin0 = min_vec[0];
        dup             v17.4s, v20.s[1]                // v128 vmin1 = min_vec[1];
        dup             v18.4s, v20.s[2]                // v128 vmin2 = min_vec[2];
        fmin            v0.4s, v0.4s, v16.4s            // vl[0] = min(vl[0], vmin0);
        fmin            v1.4s, v1.4s, v17.4s            // vl[1] = min(vl[1], vmin1);
        fmin            v2.4s, v2.4s, v18.4s            // vl[2] = min(vl[2], vmin2);
        fmin            v4.4s, v4.4s, v16.4s            // vh[0] = min(vh[0], vmin0);
        fmin            v5.4s, v5.4s, v17.4s            // vh[1] = min(vh[1], vmin1);
        fmin            v6.4s, v6.4s, v18.4s            // vh[2] = min(vh[2], vmin2);
        br              x0                              // jump to cont
endfunc

function ff_sws_min_8_f32_1001_neon, export=1, jumpable=1
        ldr             q20, [x1, #16]                  // v128 min_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v16.4s, v20.s[0]                // v128 vmin0 = min_vec[0];
        dup             v19.4s, v20.s[3]                // v128 vmin3 = min_vec[3];
        fmin            v0.4s, v0.4s, v16.4s            // vl[0] = min(vl[0], vmin0);
        fmin            v3.4s, v3.4s, v19.4s            // vl[3] = min(vl[3], vmin3);
        fmin            v4.4s, v4.4s, v16.4s            // vh[0] = min(vh[0], vmin0);
        fmin            v7.4s, v7.4s, v19.4s            // vh[3] = min(vh[3], vmin3);
        br              x0                              // jump to cont
endfunc

function ff_sws_min_8_f32_1110_neon, export=1, jumpable=1
        ldr             q20, [x1, #16]                  // v128 min_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v17.4s, v20.s[1]                // v128 vmin1 = min_vec[1];
        dup             v18.4s, v20.s[2]                // v128 vmin2 = min_vec[2];
        dup             v19.4s, v20.s[3]                // v128 vmin3 = min_vec[3];
        fmin            v1.4s, v1.4s, v17.4s            // vl[1] = min(vl[1], vmin1);
        fmin            v2.4s, v2.4s, v18.4s            // vl[2] = min(vl[2], vmin2);
        fmin            v3.4s, v3.4s, v19.4s            // vl[3] = min(vl[3], vmin3);
        fmin            v5.4s, v5.4s, v17.4s            // vh[1] = min(vh[1], vmin1);
        fmin            v6.4s, v6.4s, v18.4s            // vh[2] = min(vh[2], vmin2);
        fmin            v7.4s, v7.4s, v19.4s            // vh[3] = min(vh[3], vmin3);
        br              x0                              // jump to cont
endfunc

function ff_sws_min_8_f32_1111_neon, export=1, jumpable=1
        ldr             q20, [x1, #16]                  // v128 min_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v16.4s, v20.s[0]                // v128 vmin0 = min_vec[0];
        dup             v17.4s, v20.s[1]                // v128 vmin1 = min_vec[1];
        dup             v18.4s, v20.s[2]                // v128 vmin2 = min_vec[2];
        dup             v19.4s, v20.s[3]                // v128 vmin3 = min_vec[3];
        fmin            v0.4s, v0.4s, v16.4s            // vl[0] = min(vl[0], vmin0);
        fmin            v1.4s, v1.4s, v17.4s            // vl[1] = min(vl[1], vmin1);
        fmin            v2.4s, v2.4s, v18.4s            // vl[2] = min(vl[2], vmin2);
        fmin            v3.4s, v3.4s, v19.4s            // vl[3] = min(vl[3], vmin3);
        fmin            v4.4s, v4.4s, v16.4s            // vh[0] = min(vh[0], vmin0);
        fmin            v5.4s, v5.4s, v17.4s            // vh[1] = min(vh[1], vmin1);
        fmin            v6.4s, v6.4s, v18.4s            // vh[2] = min(vh[2], vmin2);
        fmin            v7.4s, v7.4s, v19.4s            // vh[3] = min(vh[3], vmin3);
        br              x0                              // jump to cont
endfunc

function ff_sws_max_8_f32_0001_neon, export=1, jumpable=1
        ldr             q20, [x1, #16]                  // v128 max_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v16.4s, v20.s[0]                // v128 vmax0 = max_vec[0];
        fmax            v0.4s, v0.4s, v16.4s            // vl[0] = max(vl[0], vmax0);
        fmax            v4.4s, v4.4s, v16.4s            // vh[0] = max(vh[0], vmax0);
        br              x0                              // jump to cont
endfunc

function ff_sws_max_8_f32_0011_neon, export=1, jumpable=1
        ldr             q20, [x1, #16]                  // v128 max_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v16.4s, v20.s[0]                // v128 vmax0 = max_vec[0];
        dup             v17.4s, v20.s[1]                // v128 vmax1 = max_vec[1];
        fmax            v0.4s, v0.4s, v16.4s            // vl[0] = max(vl[0], vmax0);
        fmax            v1.4s, v1.4s, v17.4s            // vl[1] = max(vl[1], vmax1);
        fmax            v4.4s, v4.4s, v16.4s            // vh[0] = max(vh[0], vmax0);
        fmax            v5.4s, v5.4s, v17.4s            // vh[1] = max(vh[1], vmax1);
        br              x0                              // jump to cont
endfunc

function ff_sws_max_8_f32_0111_neon, export=1, jumpable=1
        ldr             q20, [x1, #16]                  // v128 max_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v16.4s, v20.s[0]                // v128 vmax0 = max_vec[0];
        dup             v17.4s, v20.s[1]                // v128 vmax1 = max_vec[1];
        dup             v18.4s, v20.s[2]                // v128 vmax2 = max_vec[2];
        fmax            v0.4s, v0.4s, v16.4s            // vl[0] = max(vl[0], vmax0);
        fmax            v1.4s, v1.4s, v17.4s            // vl[1] = max(vl[1], vmax1);
        fmax            v2.4s, v2.4s, v18.4s            // vl[2] = max(vl[2], vmax2);
        fmax            v4.4s, v4.4s, v16.4s            // vh[0] = max(vh[0], vmax0);
        fmax            v5.4s, v5.4s, v17.4s            // vh[1] = max(vh[1], vmax1);
        fmax            v6.4s, v6.4s, v18.4s            // vh[2] = max(vh[2], vmax2);
        br              x0                              // jump to cont
endfunc

function ff_sws_max_8_f32_1001_neon, export=1, jumpable=1
        ldr             q20, [x1, #16]                  // v128 max_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v16.4s, v20.s[0]                // v128 vmax0 = max_vec[0];
        dup             v19.4s, v20.s[3]                // v128 vmax3 = max_vec[3];
        fmax            v0.4s, v0.4s, v16.4s            // vl[0] = max(vl[0], vmax0);
        fmax            v3.4s, v3.4s, v19.4s            // vl[3] = max(vl[3], vmax3);
        fmax            v4.4s, v4.4s, v16.4s            // vh[0] = max(vh[0], vmax0);
        fmax            v7.4s, v7.4s, v19.4s            // vh[3] = max(vh[3], vmax3);
        br              x0                              // jump to cont
endfunc

function ff_sws_max_8_f32_1111_neon, export=1, jumpable=1
        ldr             q20, [x1, #16]                  // v128 max_vec = impl->priv.v128;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        dup             v16.4s, v20.s[0]                // v128 vmax0 = max_vec[0];
        dup             v17.4s, v20.s[1]                // v128 vmax1 = max_vec[1];
        dup             v18.4s, v20.s[2]                // v128 vmax2 = max_vec[2];
        dup             v19.4s, v20.s[3]                // v128 vmax3 = max_vec[3];
        fmax            v0.4s, v0.4s, v16.4s            // vl[0] = max(vl[0], vmax0);
        fmax            v1.4s, v1.4s, v17.4s            // vl[1] = max(vl[1], vmax1);
        fmax            v2.4s, v2.4s, v18.4s            // vl[2] = max(vl[2], vmax2);
        fmax            v3.4s, v3.4s, v19.4s            // vl[3] = max(vl[3], vmax3);
        fmax            v4.4s, v4.4s, v16.4s            // vh[0] = max(vh[0], vmax0);
        fmax            v5.4s, v5.4s, v17.4s            // vh[1] = max(vh[1], vmax1);
        fmax            v6.4s, v6.4s, v18.4s            // vh[2] = max(vh[2], vmax2);
        fmax            v7.4s, v7.4s, v19.4s            // vh[3] = max(vh[3], vmax3);
        br              x0                              // jump to cont
endfunc

function ff_sws_scale_8_u32_0001_neon, export=1, jumpable=1
        add             x16, x1, #16                    // v128 *scale_vec_ptr = &impl->priv;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1r            {v16.4s}, [x16]                 // v128 scale_vec = broadcast(*scale_vec_ptr);
        mul             v0.4s, v0.4s, v16.4s            // vl[0] *= scale_vec;
        mul             v4.4s, v4.4s, v16.4s            // vh[0] *= scale_vec;
        br              x0                              // jump to cont
endfunc

function ff_sws_scale_8_u32_0111_neon, export=1, jumpable=1
        add             x16, x1, #16                    // v128 *scale_vec_ptr = &impl->priv;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1r            {v16.4s}, [x16]                 // v128 scale_vec = broadcast(*scale_vec_ptr);
        mul             v0.4s, v0.4s, v16.4s            // vl[0] *= scale_vec;
        mul             v1.4s, v1.4s, v16.4s            // vl[1] *= scale_vec;
        mul             v2.4s, v2.4s, v16.4s            // vl[2] *= scale_vec;
        mul             v4.4s, v4.4s, v16.4s            // vh[0] *= scale_vec;
        mul             v5.4s, v5.4s, v16.4s            // vh[1] *= scale_vec;
        mul             v6.4s, v6.4s, v16.4s            // vh[2] *= scale_vec;
        br              x0                              // jump to cont
endfunc

function ff_sws_scale_8_f32_0001_neon, export=1, jumpable=1
        add             x16, x1, #16                    // v128 *scale_vec_ptr = &impl->priv;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1r            {v16.4s}, [x16]                 // v128 scale_vec = broadcast(*scale_vec_ptr);
        fmul            v0.4s, v0.4s, v16.4s            // vl[0] *= scale_vec;
        fmul            v4.4s, v4.4s, v16.4s            // vh[0] *= scale_vec;
        br              x0                              // jump to cont
endfunc

function ff_sws_scale_8_f32_0011_neon, export=1, jumpable=1
        add             x16, x1, #16                    // v128 *scale_vec_ptr = &impl->priv;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1r            {v16.4s}, [x16]                 // v128 scale_vec = broadcast(*scale_vec_ptr);
        fmul            v0.4s, v0.4s, v16.4s            // vl[0] *= scale_vec;
        fmul            v1.4s, v1.4s, v16.4s            // vl[1] *= scale_vec;
        fmul            v4.4s, v4.4s, v16.4s            // vh[0] *= scale_vec;
        fmul            v5.4s, v5.4s, v16.4s            // vh[1] *= scale_vec;
        br              x0                              // jump to cont
endfunc

function ff_sws_scale_8_f32_0111_neon, export=1, jumpable=1
        add             x16, x1, #16                    // v128 *scale_vec_ptr = &impl->priv;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1r            {v16.4s}, [x16]                 // v128 scale_vec = broadcast(*scale_vec_ptr);
        fmul            v0.4s, v0.4s, v16.4s            // vl[0] *= scale_vec;
        fmul            v1.4s, v1.4s, v16.4s            // vl[1] *= scale_vec;
        fmul            v2.4s, v2.4s, v16.4s            // vl[2] *= scale_vec;
        fmul            v4.4s, v4.4s, v16.4s            // vh[0] *= scale_vec;
        fmul            v5.4s, v5.4s, v16.4s            // vh[1] *= scale_vec;
        fmul            v6.4s, v6.4s, v16.4s            // vh[2] *= scale_vec;
        br              x0                              // jump to cont
endfunc

function ff_sws_scale_8_f32_1110_neon, export=1, jumpable=1
        add             x16, x1, #16                    // v128 *scale_vec_ptr = &impl->priv;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1r            {v16.4s}, [x16]                 // v128 scale_vec = broadcast(*scale_vec_ptr);
        fmul            v1.4s, v1.4s, v16.4s            // vl[1] *= scale_vec;
        fmul            v2.4s, v2.4s, v16.4s            // vl[2] *= scale_vec;
        fmul            v3.4s, v3.4s, v16.4s            // vl[3] *= scale_vec;
        fmul            v5.4s, v5.4s, v16.4s            // vh[1] *= scale_vec;
        fmul            v6.4s, v6.4s, v16.4s            // vh[2] *= scale_vec;
        fmul            v7.4s, v7.4s, v16.4s            // vh[3] *= scale_vec;
        br              x0                              // jump to cont
endfunc

function ff_sws_scale_8_f32_1111_neon, export=1, jumpable=1
        add             x16, x1, #16                    // v128 *scale_vec_ptr = &impl->priv;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1r            {v16.4s}, [x16]                 // v128 scale_vec = broadcast(*scale_vec_ptr);
        fmul            v0.4s, v0.4s, v16.4s            // vl[0] *= scale_vec;
        fmul            v1.4s, v1.4s, v16.4s            // vl[1] *= scale_vec;
        fmul            v2.4s, v2.4s, v16.4s            // vl[2] *= scale_vec;
        fmul            v3.4s, v3.4s, v16.4s            // vl[3] *= scale_vec;
        fmul            v4.4s, v4.4s, v16.4s            // vh[0] *= scale_vec;
        fmul            v5.4s, v5.4s, v16.4s            // vh[1] *= scale_vec;
        fmul            v6.4s, v6.4s, v16.4s            // vh[2] *= scale_vec;
        fmul            v7.4s, v7.4s, v16.4s            // vh[3] *= scale_vec;
        br              x0                              // jump to cont
endfunc

function ff_sws_scale_16_u8_0001_neon, export=1, jumpable=1
        add             x16, x1, #16                    // v128 *scale_vec_ptr = &impl->priv;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1r            {v16.16b}, [x16]                // v128 scale_vec = broadcast(*scale_vec_ptr);
        mul             v0.16b, v0.16b, v16.16b         // vl[0] *= scale_vec;
        br              x0                              // jump to cont
endfunc

function ff_sws_scale_16_u8_0111_neon, export=1, jumpable=1
        add             x16, x1, #16                    // v128 *scale_vec_ptr = &impl->priv;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1r            {v16.16b}, [x16]                // v128 scale_vec = broadcast(*scale_vec_ptr);
        mul             v0.16b, v0.16b, v16.16b         // vl[0] *= scale_vec;
        mul             v1.16b, v1.16b, v16.16b         // vl[1] *= scale_vec;
        mul             v2.16b, v2.16b, v16.16b         // vl[2] *= scale_vec;
        br              x0                              // jump to cont
endfunc

function ff_sws_scale_16_u16_0001_neon, export=1, jumpable=1
        add             x16, x1, #16                    // v128 *scale_vec_ptr = &impl->priv;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1r            {v16.8h}, [x16]                 // v128 scale_vec = broadcast(*scale_vec_ptr);
        mul             v0.8h, v0.8h, v16.8h            // vl[0] *= scale_vec;
        mul             v4.8h, v4.8h, v16.8h            // vh[0] *= scale_vec;
        br              x0                              // jump to cont
endfunc

function ff_sws_scale_16_u16_0111_neon, export=1, jumpable=1
        add             x16, x1, #16                    // v128 *scale_vec_ptr = &impl->priv;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1r            {v16.8h}, [x16]                 // v128 scale_vec = broadcast(*scale_vec_ptr);
        mul             v0.8h, v0.8h, v16.8h            // vl[0] *= scale_vec;
        mul             v1.8h, v1.8h, v16.8h            // vl[1] *= scale_vec;
        mul             v2.8h, v2.8h, v16.8h            // vl[2] *= scale_vec;
        mul             v4.8h, v4.8h, v16.8h            // vh[0] *= scale_vec;
        mul             v5.8h, v5.8h, v16.8h            // vh[1] *= scale_vec;
        mul             v6.8h, v6.8h, v16.8h            // vh[2] *= scale_vec;
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_000000000f_0_8_f32_0001_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s}, [x16]                 // coeff_veclist = *vcoeff_ptr;
        mov             v16.16b, v0.16b                 // vsrc[0] = vl[0];
        fmul            v21.4s, v16.4s, v24.s[1]        // vtmp[1] = vsrc[0] * vc[0][1];
        dup             v0.4s, v24.s[0]                 // vl[0]  = broadcast(vc[0][0]);
        fadd            v0.4s, v0.4s, v21.4s            // vl[0] += vtmp[1];
        mov             v16.16b, v4.16b                 // vsrc[0] = vh[0];
        fmul            v21.4s, v16.4s, v24.s[1]        // vtmp[1] = vsrc[0] * vc[0][1];
        dup             v4.4s, v24.s[0]                 // vh[0]  = broadcast(vc[0][0]);
        fadd            v4.4s, v4.4s, v21.4s            // vh[0] += vtmp[1];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_000000000f_1_8_f32_0001_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s}, [x16]                 // coeff_veclist = *vcoeff_ptr;
        mov             v16.16b, v0.16b                 // vsrc[0] = vl[0];
        dup             v0.4s, v24.s[0]                 // vl[0]  = broadcast(vc[0][0]);
        fmla            v0.4s, v16.4s, v24.s[1]         // vl[0] += vsrc[0] * vc[0][1];
        mov             v16.16b, v4.16b                 // vsrc[0] = vh[0];
        dup             v4.4s, v24.s[0]                 // vh[0]  = broadcast(vc[0][0]);
        fmla            v4.4s, v16.4s, v24.s[1]         // vh[0] += vsrc[0] * vc[0][1];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_00000000fc_0_8_f32_0001_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s}, [x16]                 // coeff_veclist = *vcoeff_ptr;
        fmul            v21.4s, v1.4s, v24.s[1]         // vtmp[1] = vsrc[1] * vc[0][1];
        fmul            v22.4s, v2.4s, v24.s[2]         // vtmp[2] = vsrc[2] * vc[0][2];
        fmul            v0.4s, v0.4s, v24.s[0]          // vl[0]  = vsrc[0] * vc[0][0];
        fadd            v0.4s, v0.4s, v21.4s            // vl[0] += vtmp[1];
        fadd            v0.4s, v0.4s, v22.4s            // vl[0] += vtmp[2];
        fmul            v21.4s, v5.4s, v24.s[1]         // vtmp[1] = vsrc[1] * vc[0][1];
        fmul            v22.4s, v6.4s, v24.s[2]         // vtmp[2] = vsrc[2] * vc[0][2];
        fmul            v4.4s, v4.4s, v24.s[0]          // vh[0]  = vsrc[0] * vc[0][0];
        fadd            v4.4s, v4.4s, v21.4s            // vh[0] += vtmp[1];
        fadd            v4.4s, v4.4s, v22.4s            // vh[0] += vtmp[2];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_00000000fc_1_8_f32_0001_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s}, [x16]                 // coeff_veclist = *vcoeff_ptr;
        fmul            v0.4s, v0.4s, v24.s[0]          // vl[0]  = vsrc[0] * vc[0][0];
        fmla            v0.4s, v1.4s, v24.s[1]          // vl[0] += vsrc[1] * vc[0][1];
        fmla            v0.4s, v2.4s, v24.s[2]          // vl[0] += vsrc[2] * vc[0][2];
        fmul            v4.4s, v4.4s, v24.s[0]          // vh[0]  = vsrc[0] * vc[0][0];
        fmla            v4.4s, v5.4s, v24.s[1]          // vh[0] += vsrc[1] * vc[0][1];
        fmla            v4.4s, v6.4s, v24.s[2]          // vh[0] += vsrc[2] * vc[0][2];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_00000000ff_0_8_f32_0001_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s}, [x16]                 // coeff_veclist = *vcoeff_ptr;
        mov             v16.16b, v0.16b                 // vsrc[0] = vl[0];
        fmul            v21.4s, v16.4s, v24.s[1]        // vtmp[1] = vsrc[0] * vc[0][1];
        fmul            v22.4s, v1.4s, v24.s[2]         // vtmp[2] = vsrc[1] * vc[0][2];
        fmul            v23.4s, v2.4s, v24.s[3]         // vtmp[3] = vsrc[2] * vc[0][3];
        dup             v0.4s, v24.s[0]                 // vl[0]  = broadcast(vc[0][0]);
        fadd            v0.4s, v0.4s, v21.4s            // vl[0] += vtmp[1];
        fadd            v0.4s, v0.4s, v22.4s            // vl[0] += vtmp[2];
        fadd            v0.4s, v0.4s, v23.4s            // vl[0] += vtmp[3];
        mov             v16.16b, v4.16b                 // vsrc[0] = vh[0];
        fmul            v21.4s, v16.4s, v24.s[1]        // vtmp[1] = vsrc[0] * vc[0][1];
        fmul            v22.4s, v5.4s, v24.s[2]         // vtmp[2] = vsrc[1] * vc[0][2];
        fmul            v23.4s, v6.4s, v24.s[3]         // vtmp[3] = vsrc[2] * vc[0][3];
        dup             v4.4s, v24.s[0]                 // vh[0]  = broadcast(vc[0][0]);
        fadd            v4.4s, v4.4s, v21.4s            // vh[0] += vtmp[1];
        fadd            v4.4s, v4.4s, v22.4s            // vh[0] += vtmp[2];
        fadd            v4.4s, v4.4s, v23.4s            // vh[0] += vtmp[3];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_00000000ff_1_8_f32_0001_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s}, [x16]                 // coeff_veclist = *vcoeff_ptr;
        mov             v16.16b, v0.16b                 // vsrc[0] = vl[0];
        dup             v0.4s, v24.s[0]                 // vl[0]  = broadcast(vc[0][0]);
        fmla            v0.4s, v16.4s, v24.s[1]         // vl[0] += vsrc[0] * vc[0][1];
        fmla            v0.4s, v1.4s, v24.s[2]          // vl[0] += vsrc[1] * vc[0][2];
        fmla            v0.4s, v2.4s, v24.s[3]          // vl[0] += vsrc[2] * vc[0][3];
        mov             v16.16b, v4.16b                 // vsrc[0] = vh[0];
        dup             v4.4s, v24.s[0]                 // vh[0]  = broadcast(vc[0][0]);
        fmla            v4.4s, v16.4s, v24.s[1]         // vh[0] += vsrc[0] * vc[0][1];
        fmla            v4.4s, v5.4s, v24.s[2]          // vh[0] += vsrc[1] * vc[0][2];
        fmla            v4.4s, v6.4s, v24.s[3]          // vh[0] += vsrc[2] * vc[0][3];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_000000c000_0_8_f32_0010_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s}, [x16]                 // coeff_veclist = *vcoeff_ptr;
        fmul            v1.4s, v1.4s, v24.s[0]          // vl[1]  = vsrc[1] * vc[0][0];
        fmul            v5.4s, v5.4s, v24.s[0]          // vh[1]  = vsrc[1] * vc[0][0];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_000000c000_1_8_f32_0010_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s}, [x16]                 // coeff_veclist = *vcoeff_ptr;
        fmul            v1.4s, v1.4s, v24.s[0]          // vl[1]  = vsrc[1] * vc[0][0];
        fmul            v5.4s, v5.4s, v24.s[0]          // vh[1]  = vsrc[1] * vc[0][0];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_000373dcc7_0_8_f32_0111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s, v25.4s, v26.4s}, [x16] // coeff_veclist = *vcoeff_ptr;
        mov             v16.16b, v0.16b                 // vsrc[0] = vl[0];
        mov             v17.16b, v1.16b                 // vsrc[1] = vl[1];
        fmul            v22.4s, v2.4s, v24.s[2]         // vtmp[2] = vsrc[2] * vc[0][2];
        dup             v0.4s, v24.s[0]                 // vl[0]  = broadcast(vc[0][0]);
        fadd            v0.4s, v0.4s, v16.4s            // vl[0] += vsrc[1];
        fadd            v0.4s, v0.4s, v22.4s            // vl[0] += vtmp[2];
        fmul            v21.4s, v17.4s, v25.s[1]        // vtmp[1] = vsrc[1] * vc[1][1];
        fmul            v22.4s, v2.4s, v25.s[2]         // vtmp[2] = vsrc[2] * vc[1][2];
        dup             v1.4s, v24.s[3]                 // vl[1]  = broadcast(vc[0][3]);
        fadd            v1.4s, v1.4s, v16.4s            // vl[1] += vsrc[0];
        fadd            v1.4s, v1.4s, v21.4s            // vl[1] += vtmp[1];
        fadd            v1.4s, v1.4s, v22.4s            // vl[1] += vtmp[2];
        fmul            v21.4s, v17.4s, v26.s[1]        // vtmp[1] = vsrc[1] * vc[2][1];
        dup             v2.4s, v25.s[3]                 // vl[2]  = broadcast(vc[1][3]);
        fadd            v2.4s, v2.4s, v16.4s            // vl[2] += vsrc[0];
        fadd            v2.4s, v2.4s, v21.4s            // vl[2] += vtmp[1];
        mov             v16.16b, v4.16b                 // vsrc[0] = vh[0];
        mov             v17.16b, v5.16b                 // vsrc[1] = vh[1];
        fmul            v22.4s, v6.4s, v24.s[2]         // vtmp[2] = vsrc[2] * vc[0][2];
        dup             v4.4s, v24.s[0]                 // vh[0]  = broadcast(vc[0][0]);
        fadd            v4.4s, v4.4s, v16.4s            // vh[0] += vsrc[1];
        fadd            v4.4s, v4.4s, v22.4s            // vh[0] += vtmp[2];
        fmul            v21.4s, v17.4s, v25.s[1]        // vtmp[1] = vsrc[1] * vc[1][1];
        fmul            v22.4s, v6.4s, v25.s[2]         // vtmp[2] = vsrc[2] * vc[1][2];
        dup             v5.4s, v24.s[3]                 // vh[1]  = broadcast(vc[0][3]);
        fadd            v5.4s, v5.4s, v16.4s            // vh[1] += vsrc[0];
        fadd            v5.4s, v5.4s, v21.4s            // vh[1] += vtmp[1];
        fadd            v5.4s, v5.4s, v22.4s            // vh[1] += vtmp[2];
        fmul            v21.4s, v17.4s, v26.s[1]        // vtmp[1] = vsrc[1] * vc[2][1];
        dup             v6.4s, v25.s[3]                 // vh[2]  = broadcast(vc[1][3]);
        fadd            v6.4s, v6.4s, v16.4s            // vh[2] += vsrc[0];
        fadd            v6.4s, v6.4s, v21.4s            // vh[2] += vtmp[1];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_000373dcc7_1_8_f32_0111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s, v25.4s, v26.4s}, [x16] // coeff_veclist = *vcoeff_ptr;
        mov             v16.16b, v0.16b                 // vsrc[0] = vl[0];
        mov             v17.16b, v1.16b                 // vsrc[1] = vl[1];
        dup             v0.4s, v24.s[0]                 // vl[0]  = broadcast(vc[0][0]);
        fmla            v0.4s, v16.4s, v24.s[1]         // vl[0] += vsrc[0] * vc[0][1];
        fmla            v0.4s, v2.4s, v24.s[2]          // vl[0] += vsrc[2] * vc[0][2];
        dup             v1.4s, v24.s[3]                 // vl[1]  = broadcast(vc[0][3]);
        fmla            v1.4s, v16.4s, v25.s[0]         // vl[1] += vsrc[0] * vc[1][0];
        fmla            v1.4s, v17.4s, v25.s[1]         // vl[1] += vsrc[1] * vc[1][1];
        fmla            v1.4s, v2.4s, v25.s[2]          // vl[1] += vsrc[2] * vc[1][2];
        dup             v2.4s, v25.s[3]                 // vl[2]  = broadcast(vc[1][3]);
        fmla            v2.4s, v16.4s, v26.s[0]         // vl[2] += vsrc[0] * vc[2][0];
        fmla            v2.4s, v17.4s, v26.s[1]         // vl[2] += vsrc[1] * vc[2][1];
        mov             v16.16b, v4.16b                 // vsrc[0] = vh[0];
        mov             v17.16b, v5.16b                 // vsrc[1] = vh[1];
        dup             v4.4s, v24.s[0]                 // vh[0]  = broadcast(vc[0][0]);
        fmla            v4.4s, v16.4s, v24.s[1]         // vh[0] += vsrc[0] * vc[0][1];
        fmla            v4.4s, v6.4s, v24.s[2]          // vh[0] += vsrc[2] * vc[0][2];
        dup             v5.4s, v24.s[3]                 // vh[1]  = broadcast(vc[0][3]);
        fmla            v5.4s, v16.4s, v25.s[0]         // vh[1] += vsrc[0] * vc[1][0];
        fmla            v5.4s, v17.4s, v25.s[1]         // vh[1] += vsrc[1] * vc[1][1];
        fmla            v5.4s, v6.4s, v25.s[2]          // vh[1] += vsrc[2] * vc[1][2];
        dup             v6.4s, v25.s[3]                 // vh[2]  = broadcast(vc[1][3]);
        fmla            v6.4s, v16.4s, v26.s[0]         // vh[2] += vsrc[0] * vc[2][0];
        fmla            v6.4s, v17.4s, v26.s[1]         // vh[2] += vsrc[1] * vc[2][1];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_0003f3fccf_0_8_f32_0111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s, v25.4s, v26.4s}, [x16] // coeff_veclist = *vcoeff_ptr;
        mov             v16.16b, v0.16b                 // vsrc[0] = vl[0];
        mov             v17.16b, v1.16b                 // vsrc[1] = vl[1];
        fmul            v21.4s, v16.4s, v24.s[1]        // vtmp[1] = vsrc[0] * vc[0][1];
        fmul            v22.4s, v2.4s, v24.s[2]         // vtmp[2] = vsrc[2] * vc[0][2];
        dup             v0.4s, v24.s[0]                 // vl[0]  = broadcast(vc[0][0]);
        fadd            v0.4s, v0.4s, v21.4s            // vl[0] += vtmp[1];
        fadd            v0.4s, v0.4s, v22.4s            // vl[0] += vtmp[2];
        fmul            v20.4s, v16.4s, v25.s[0]        // vtmp[0] = vsrc[0] * vc[1][0];
        fmul            v21.4s, v17.4s, v25.s[1]        // vtmp[1] = vsrc[1] * vc[1][1];
        fmul            v22.4s, v2.4s, v25.s[2]         // vtmp[2] = vsrc[2] * vc[1][2];
        dup             v1.4s, v24.s[3]                 // vl[1]  = broadcast(vc[0][3]);
        fadd            v1.4s, v1.4s, v20.4s            // vl[1] += vtmp[0];
        fadd            v1.4s, v1.4s, v21.4s            // vl[1] += vtmp[1];
        fadd            v1.4s, v1.4s, v22.4s            // vl[1] += vtmp[2];
        fmul            v20.4s, v16.4s, v26.s[0]        // vtmp[0] = vsrc[0] * vc[2][0];
        fmul            v21.4s, v17.4s, v26.s[1]        // vtmp[1] = vsrc[1] * vc[2][1];
        dup             v2.4s, v25.s[3]                 // vl[2]  = broadcast(vc[1][3]);
        fadd            v2.4s, v2.4s, v20.4s            // vl[2] += vtmp[0];
        fadd            v2.4s, v2.4s, v21.4s            // vl[2] += vtmp[1];
        mov             v16.16b, v4.16b                 // vsrc[0] = vh[0];
        mov             v17.16b, v5.16b                 // vsrc[1] = vh[1];
        fmul            v21.4s, v16.4s, v24.s[1]        // vtmp[1] = vsrc[0] * vc[0][1];
        fmul            v22.4s, v6.4s, v24.s[2]         // vtmp[2] = vsrc[2] * vc[0][2];
        dup             v4.4s, v24.s[0]                 // vh[0]  = broadcast(vc[0][0]);
        fadd            v4.4s, v4.4s, v21.4s            // vh[0] += vtmp[1];
        fadd            v4.4s, v4.4s, v22.4s            // vh[0] += vtmp[2];
        fmul            v20.4s, v16.4s, v25.s[0]        // vtmp[0] = vsrc[0] * vc[1][0];
        fmul            v21.4s, v17.4s, v25.s[1]        // vtmp[1] = vsrc[1] * vc[1][1];
        fmul            v22.4s, v6.4s, v25.s[2]         // vtmp[2] = vsrc[2] * vc[1][2];
        dup             v5.4s, v24.s[3]                 // vh[1]  = broadcast(vc[0][3]);
        fadd            v5.4s, v5.4s, v20.4s            // vh[1] += vtmp[0];
        fadd            v5.4s, v5.4s, v21.4s            // vh[1] += vtmp[1];
        fadd            v5.4s, v5.4s, v22.4s            // vh[1] += vtmp[2];
        fmul            v20.4s, v16.4s, v26.s[0]        // vtmp[0] = vsrc[0] * vc[2][0];
        fmul            v21.4s, v17.4s, v26.s[1]        // vtmp[1] = vsrc[1] * vc[2][1];
        dup             v6.4s, v25.s[3]                 // vh[2]  = broadcast(vc[1][3]);
        fadd            v6.4s, v6.4s, v20.4s            // vh[2] += vtmp[0];
        fadd            v6.4s, v6.4s, v21.4s            // vh[2] += vtmp[1];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_0003f3fccf_1_8_f32_0111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s, v25.4s, v26.4s}, [x16] // coeff_veclist = *vcoeff_ptr;
        mov             v16.16b, v0.16b                 // vsrc[0] = vl[0];
        mov             v17.16b, v1.16b                 // vsrc[1] = vl[1];
        dup             v0.4s, v24.s[0]                 // vl[0]  = broadcast(vc[0][0]);
        fmla            v0.4s, v16.4s, v24.s[1]         // vl[0] += vsrc[0] * vc[0][1];
        fmla            v0.4s, v2.4s, v24.s[2]          // vl[0] += vsrc[2] * vc[0][2];
        dup             v1.4s, v24.s[3]                 // vl[1]  = broadcast(vc[0][3]);
        fmla            v1.4s, v16.4s, v25.s[0]         // vl[1] += vsrc[0] * vc[1][0];
        fmla            v1.4s, v17.4s, v25.s[1]         // vl[1] += vsrc[1] * vc[1][1];
        fmla            v1.4s, v2.4s, v25.s[2]          // vl[1] += vsrc[2] * vc[1][2];
        dup             v2.4s, v25.s[3]                 // vl[2]  = broadcast(vc[1][3]);
        fmla            v2.4s, v16.4s, v26.s[0]         // vl[2] += vsrc[0] * vc[2][0];
        fmla            v2.4s, v17.4s, v26.s[1]         // vl[2] += vsrc[1] * vc[2][1];
        mov             v16.16b, v4.16b                 // vsrc[0] = vh[0];
        mov             v17.16b, v5.16b                 // vsrc[1] = vh[1];
        dup             v4.4s, v24.s[0]                 // vh[0]  = broadcast(vc[0][0]);
        fmla            v4.4s, v16.4s, v24.s[1]         // vh[0] += vsrc[0] * vc[0][1];
        fmla            v4.4s, v6.4s, v24.s[2]          // vh[0] += vsrc[2] * vc[0][2];
        dup             v5.4s, v24.s[3]                 // vh[1]  = broadcast(vc[0][3]);
        fmla            v5.4s, v16.4s, v25.s[0]         // vh[1] += vsrc[0] * vc[1][0];
        fmla            v5.4s, v17.4s, v25.s[1]         // vh[1] += vsrc[1] * vc[1][1];
        fmla            v5.4s, v6.4s, v25.s[2]          // vh[1] += vsrc[2] * vc[1][2];
        dup             v6.4s, v25.s[3]                 // vh[2]  = broadcast(vc[1][3]);
        fmla            v6.4s, v16.4s, v26.s[0]         // vh[2] += vsrc[0] * vc[2][0];
        fmla            v6.4s, v17.4s, v26.s[1]         // vh[2] += vsrc[1] * vc[2][1];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_000c00c00c_0_8_f32_0111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s}, [x16]                 // coeff_veclist = *vcoeff_ptr;
        fmul            v0.4s, v0.4s, v24.s[0]          // vl[0]  = vsrc[0] * vc[0][0];
        fmul            v1.4s, v1.4s, v24.s[1]          // vl[1]  = vsrc[1] * vc[0][1];
        fmul            v2.4s, v2.4s, v24.s[2]          // vl[2]  = vsrc[2] * vc[0][2];
        fmul            v4.4s, v4.4s, v24.s[0]          // vh[0]  = vsrc[0] * vc[0][0];
        fmul            v5.4s, v5.4s, v24.s[1]          // vh[1]  = vsrc[1] * vc[0][1];
        fmul            v6.4s, v6.4s, v24.s[2]          // vh[2]  = vsrc[2] * vc[0][2];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_000c00c00c_1_8_f32_0111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s}, [x16]                 // coeff_veclist = *vcoeff_ptr;
        fmul            v0.4s, v0.4s, v24.s[0]          // vl[0]  = vsrc[0] * vc[0][0];
        fmul            v1.4s, v1.4s, v24.s[1]          // vl[1]  = vsrc[1] * vc[0][1];
        fmul            v2.4s, v2.4s, v24.s[2]          // vl[2]  = vsrc[2] * vc[0][2];
        fmul            v4.4s, v4.4s, v24.s[0]          // vh[0]  = vsrc[0] * vc[0][0];
        fmul            v5.4s, v5.4s, v24.s[1]          // vh[1]  = vsrc[1] * vc[0][1];
        fmul            v6.4s, v6.4s, v24.s[2]          // vh[2]  = vsrc[2] * vc[0][2];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_000c30cc0f_0_8_f32_0111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s, v25.4s}, [x16]         // coeff_veclist = *vcoeff_ptr;
        mov             v16.16b, v0.16b                 // vsrc[0] = vl[0];
        mov             v17.16b, v1.16b                 // vsrc[1] = vl[1];
        mov             v18.16b, v2.16b                 // vsrc[2] = vl[2];
        fmul            v21.4s, v16.4s, v24.s[1]        // vtmp[1] = vsrc[0] * vc[0][1];
        dup             v0.4s, v24.s[0]                 // vl[0]  = broadcast(vc[0][0]);
        fadd            v0.4s, v0.4s, v21.4s            // vl[0] += vtmp[1];
        fmul            v23.4s, v17.4s, v24.s[3]        // vtmp[3] = vsrc[1] * vc[0][3];
        dup             v1.4s, v24.s[2]                 // vl[1]  = broadcast(vc[0][2]);
        fadd            v1.4s, v1.4s, v23.4s            // vl[1] += vtmp[3];
        fmul            v21.4s, v18.4s, v25.s[1]        // vtmp[1] = vsrc[2] * vc[1][1];
        dup             v2.4s, v25.s[0]                 // vl[2]  = broadcast(vc[1][0]);
        fadd            v2.4s, v2.4s, v21.4s            // vl[2] += vtmp[1];
        mov             v16.16b, v4.16b                 // vsrc[0] = vh[0];
        mov             v17.16b, v5.16b                 // vsrc[1] = vh[1];
        mov             v18.16b, v6.16b                 // vsrc[2] = vh[2];
        fmul            v21.4s, v16.4s, v24.s[1]        // vtmp[1] = vsrc[0] * vc[0][1];
        dup             v4.4s, v24.s[0]                 // vh[0]  = broadcast(vc[0][0]);
        fadd            v4.4s, v4.4s, v21.4s            // vh[0] += vtmp[1];
        fmul            v23.4s, v17.4s, v24.s[3]        // vtmp[3] = vsrc[1] * vc[0][3];
        dup             v5.4s, v24.s[2]                 // vh[1]  = broadcast(vc[0][2]);
        fadd            v5.4s, v5.4s, v23.4s            // vh[1] += vtmp[3];
        fmul            v21.4s, v18.4s, v25.s[1]        // vtmp[1] = vsrc[2] * vc[1][1];
        dup             v6.4s, v25.s[0]                 // vh[2]  = broadcast(vc[1][0]);
        fadd            v6.4s, v6.4s, v21.4s            // vh[2] += vtmp[1];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_000c30cc0f_1_8_f32_0111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s, v25.4s}, [x16]         // coeff_veclist = *vcoeff_ptr;
        mov             v16.16b, v0.16b                 // vsrc[0] = vl[0];
        mov             v17.16b, v1.16b                 // vsrc[1] = vl[1];
        mov             v18.16b, v2.16b                 // vsrc[2] = vl[2];
        dup             v0.4s, v24.s[0]                 // vl[0]  = broadcast(vc[0][0]);
        fmla            v0.4s, v16.4s, v24.s[1]         // vl[0] += vsrc[0] * vc[0][1];
        dup             v1.4s, v24.s[2]                 // vl[1]  = broadcast(vc[0][2]);
        fmla            v1.4s, v17.4s, v24.s[3]         // vl[1] += vsrc[1] * vc[0][3];
        dup             v2.4s, v25.s[0]                 // vl[2]  = broadcast(vc[1][0]);
        fmla            v2.4s, v18.4s, v25.s[1]         // vl[2] += vsrc[2] * vc[1][1];
        mov             v16.16b, v4.16b                 // vsrc[0] = vh[0];
        mov             v17.16b, v5.16b                 // vsrc[1] = vh[1];
        mov             v18.16b, v6.16b                 // vsrc[2] = vh[2];
        dup             v4.4s, v24.s[0]                 // vh[0]  = broadcast(vc[0][0]);
        fmla            v4.4s, v16.4s, v24.s[1]         // vh[0] += vsrc[0] * vc[0][1];
        dup             v5.4s, v24.s[2]                 // vh[1]  = broadcast(vc[0][2]);
        fmla            v5.4s, v17.4s, v24.s[3]         // vh[1] += vsrc[1] * vc[0][3];
        dup             v6.4s, v25.s[0]                 // vh[2]  = broadcast(vc[1][0]);
        fmla            v6.4s, v18.4s, v25.s[1]         // vh[2] += vsrc[2] * vc[1][1];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_000ff3fcfc_0_8_f32_0111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s, v25.4s, v26.4s}, [x16] // coeff_veclist = *vcoeff_ptr;
        mov             v16.16b, v0.16b                 // vsrc[0] = vl[0];
        mov             v17.16b, v1.16b                 // vsrc[1] = vl[1];
        mov             v18.16b, v2.16b                 // vsrc[2] = vl[2];
        fmul            v21.4s, v17.4s, v24.s[1]        // vtmp[1] = vsrc[1] * vc[0][1];
        fmul            v22.4s, v18.4s, v24.s[2]        // vtmp[2] = vsrc[2] * vc[0][2];
        fmul            v0.4s, v16.4s, v24.s[0]         // vl[0]  = vsrc[0] * vc[0][0];
        fadd            v0.4s, v0.4s, v21.4s            // vl[0] += vtmp[1];
        fadd            v0.4s, v0.4s, v22.4s            // vl[0] += vtmp[2];
        fmul            v20.4s, v16.4s, v25.s[0]        // vtmp[0] = vsrc[0] * vc[1][0];
        fmul            v21.4s, v17.4s, v25.s[1]        // vtmp[1] = vsrc[1] * vc[1][1];
        fmul            v22.4s, v18.4s, v25.s[2]        // vtmp[2] = vsrc[2] * vc[1][2];
        dup             v1.4s, v24.s[3]                 // vl[1]  = broadcast(vc[0][3]);
        fadd            v1.4s, v1.4s, v20.4s            // vl[1] += vtmp[0];
        fadd            v1.4s, v1.4s, v21.4s            // vl[1] += vtmp[1];
        fadd            v1.4s, v1.4s, v22.4s            // vl[1] += vtmp[2];
        fmul            v20.4s, v16.4s, v26.s[0]        // vtmp[0] = vsrc[0] * vc[2][0];
        fmul            v21.4s, v17.4s, v26.s[1]        // vtmp[1] = vsrc[1] * vc[2][1];
        fmul            v22.4s, v18.4s, v26.s[2]        // vtmp[2] = vsrc[2] * vc[2][2];
        dup             v2.4s, v25.s[3]                 // vl[2]  = broadcast(vc[1][3]);
        fadd            v2.4s, v2.4s, v20.4s            // vl[2] += vtmp[0];
        fadd            v2.4s, v2.4s, v21.4s            // vl[2] += vtmp[1];
        fadd            v2.4s, v2.4s, v22.4s            // vl[2] += vtmp[2];
        mov             v16.16b, v4.16b                 // vsrc[0] = vh[0];
        mov             v17.16b, v5.16b                 // vsrc[1] = vh[1];
        mov             v18.16b, v6.16b                 // vsrc[2] = vh[2];
        fmul            v21.4s, v17.4s, v24.s[1]        // vtmp[1] = vsrc[1] * vc[0][1];
        fmul            v22.4s, v18.4s, v24.s[2]        // vtmp[2] = vsrc[2] * vc[0][2];
        fmul            v4.4s, v16.4s, v24.s[0]         // vh[0]  = vsrc[0] * vc[0][0];
        fadd            v4.4s, v4.4s, v21.4s            // vh[0] += vtmp[1];
        fadd            v4.4s, v4.4s, v22.4s            // vh[0] += vtmp[2];
        fmul            v20.4s, v16.4s, v25.s[0]        // vtmp[0] = vsrc[0] * vc[1][0];
        fmul            v21.4s, v17.4s, v25.s[1]        // vtmp[1] = vsrc[1] * vc[1][1];
        fmul            v22.4s, v18.4s, v25.s[2]        // vtmp[2] = vsrc[2] * vc[1][2];
        dup             v5.4s, v24.s[3]                 // vh[1]  = broadcast(vc[0][3]);
        fadd            v5.4s, v5.4s, v20.4s            // vh[1] += vtmp[0];
        fadd            v5.4s, v5.4s, v21.4s            // vh[1] += vtmp[1];
        fadd            v5.4s, v5.4s, v22.4s            // vh[1] += vtmp[2];
        fmul            v20.4s, v16.4s, v26.s[0]        // vtmp[0] = vsrc[0] * vc[2][0];
        fmul            v21.4s, v17.4s, v26.s[1]        // vtmp[1] = vsrc[1] * vc[2][1];
        fmul            v22.4s, v18.4s, v26.s[2]        // vtmp[2] = vsrc[2] * vc[2][2];
        dup             v6.4s, v25.s[3]                 // vh[2]  = broadcast(vc[1][3]);
        fadd            v6.4s, v6.4s, v20.4s            // vh[2] += vtmp[0];
        fadd            v6.4s, v6.4s, v21.4s            // vh[2] += vtmp[1];
        fadd            v6.4s, v6.4s, v22.4s            // vh[2] += vtmp[2];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_000ff3fcfc_1_8_f32_0111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s, v25.4s, v26.4s}, [x16] // coeff_veclist = *vcoeff_ptr;
        mov             v16.16b, v0.16b                 // vsrc[0] = vl[0];
        mov             v17.16b, v1.16b                 // vsrc[1] = vl[1];
        mov             v18.16b, v2.16b                 // vsrc[2] = vl[2];
        fmul            v0.4s, v16.4s, v24.s[0]         // vl[0]  = vsrc[0] * vc[0][0];
        fmla            v0.4s, v17.4s, v24.s[1]         // vl[0] += vsrc[1] * vc[0][1];
        fmla            v0.4s, v18.4s, v24.s[2]         // vl[0] += vsrc[2] * vc[0][2];
        dup             v1.4s, v24.s[3]                 // vl[1]  = broadcast(vc[0][3]);
        fmla            v1.4s, v16.4s, v25.s[0]         // vl[1] += vsrc[0] * vc[1][0];
        fmla            v1.4s, v17.4s, v25.s[1]         // vl[1] += vsrc[1] * vc[1][1];
        fmla            v1.4s, v18.4s, v25.s[2]         // vl[1] += vsrc[2] * vc[1][2];
        dup             v2.4s, v25.s[3]                 // vl[2]  = broadcast(vc[1][3]);
        fmla            v2.4s, v16.4s, v26.s[0]         // vl[2] += vsrc[0] * vc[2][0];
        fmla            v2.4s, v17.4s, v26.s[1]         // vl[2] += vsrc[1] * vc[2][1];
        fmla            v2.4s, v18.4s, v26.s[2]         // vl[2] += vsrc[2] * vc[2][2];
        mov             v16.16b, v4.16b                 // vsrc[0] = vh[0];
        mov             v17.16b, v5.16b                 // vsrc[1] = vh[1];
        mov             v18.16b, v6.16b                 // vsrc[2] = vh[2];
        fmul            v4.4s, v16.4s, v24.s[0]         // vh[0]  = vsrc[0] * vc[0][0];
        fmla            v4.4s, v17.4s, v24.s[1]         // vh[0] += vsrc[1] * vc[0][1];
        fmla            v4.4s, v18.4s, v24.s[2]         // vh[0] += vsrc[2] * vc[0][2];
        dup             v5.4s, v24.s[3]                 // vh[1]  = broadcast(vc[0][3]);
        fmla            v5.4s, v16.4s, v25.s[0]         // vh[1] += vsrc[0] * vc[1][0];
        fmla            v5.4s, v17.4s, v25.s[1]         // vh[1] += vsrc[1] * vc[1][1];
        fmla            v5.4s, v18.4s, v25.s[2]         // vh[1] += vsrc[2] * vc[1][2];
        dup             v6.4s, v25.s[3]                 // vh[2]  = broadcast(vc[1][3]);
        fmla            v6.4s, v16.4s, v26.s[0]         // vh[2] += vsrc[0] * vc[2][0];
        fmla            v6.4s, v17.4s, v26.s[1]         // vh[2] += vsrc[1] * vc[2][1];
        fmla            v6.4s, v18.4s, v26.s[2]         // vh[2] += vsrc[2] * vc[2][2];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_000ff3fcff_0_8_f32_0111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s, v25.4s, v26.4s}, [x16] // coeff_veclist = *vcoeff_ptr;
        mov             v16.16b, v0.16b                 // vsrc[0] = vl[0];
        mov             v17.16b, v1.16b                 // vsrc[1] = vl[1];
        mov             v18.16b, v2.16b                 // vsrc[2] = vl[2];
        fmul            v21.4s, v16.4s, v24.s[1]        // vtmp[1] = vsrc[0] * vc[0][1];
        fmul            v22.4s, v17.4s, v24.s[2]        // vtmp[2] = vsrc[1] * vc[0][2];
        fmul            v23.4s, v18.4s, v24.s[3]        // vtmp[3] = vsrc[2] * vc[0][3];
        dup             v0.4s, v24.s[0]                 // vl[0]  = broadcast(vc[0][0]);
        fadd            v0.4s, v0.4s, v21.4s            // vl[0] += vtmp[1];
        fadd            v0.4s, v0.4s, v22.4s            // vl[0] += vtmp[2];
        fadd            v0.4s, v0.4s, v23.4s            // vl[0] += vtmp[3];
        fmul            v21.4s, v16.4s, v25.s[1]        // vtmp[1] = vsrc[0] * vc[1][1];
        fmul            v22.4s, v17.4s, v25.s[2]        // vtmp[2] = vsrc[1] * vc[1][2];
        fmul            v23.4s, v18.4s, v25.s[3]        // vtmp[3] = vsrc[2] * vc[1][3];
        dup             v1.4s, v25.s[0]                 // vl[1]  = broadcast(vc[1][0]);
        fadd            v1.4s, v1.4s, v21.4s            // vl[1] += vtmp[1];
        fadd            v1.4s, v1.4s, v22.4s            // vl[1] += vtmp[2];
        fadd            v1.4s, v1.4s, v23.4s            // vl[1] += vtmp[3];
        fmul            v21.4s, v16.4s, v26.s[1]        // vtmp[1] = vsrc[0] * vc[2][1];
        fmul            v22.4s, v17.4s, v26.s[2]        // vtmp[2] = vsrc[1] * vc[2][2];
        fmul            v23.4s, v18.4s, v26.s[3]        // vtmp[3] = vsrc[2] * vc[2][3];
        dup             v2.4s, v26.s[0]                 // vl[2]  = broadcast(vc[2][0]);
        fadd            v2.4s, v2.4s, v21.4s            // vl[2] += vtmp[1];
        fadd            v2.4s, v2.4s, v22.4s            // vl[2] += vtmp[2];
        fadd            v2.4s, v2.4s, v23.4s            // vl[2] += vtmp[3];
        mov             v16.16b, v4.16b                 // vsrc[0] = vh[0];
        mov             v17.16b, v5.16b                 // vsrc[1] = vh[1];
        mov             v18.16b, v6.16b                 // vsrc[2] = vh[2];
        fmul            v21.4s, v16.4s, v24.s[1]        // vtmp[1] = vsrc[0] * vc[0][1];
        fmul            v22.4s, v17.4s, v24.s[2]        // vtmp[2] = vsrc[1] * vc[0][2];
        fmul            v23.4s, v18.4s, v24.s[3]        // vtmp[3] = vsrc[2] * vc[0][3];
        dup             v4.4s, v24.s[0]                 // vh[0]  = broadcast(vc[0][0]);
        fadd            v4.4s, v4.4s, v21.4s            // vh[0] += vtmp[1];
        fadd            v4.4s, v4.4s, v22.4s            // vh[0] += vtmp[2];
        fadd            v4.4s, v4.4s, v23.4s            // vh[0] += vtmp[3];
        fmul            v21.4s, v16.4s, v25.s[1]        // vtmp[1] = vsrc[0] * vc[1][1];
        fmul            v22.4s, v17.4s, v25.s[2]        // vtmp[2] = vsrc[1] * vc[1][2];
        fmul            v23.4s, v18.4s, v25.s[3]        // vtmp[3] = vsrc[2] * vc[1][3];
        dup             v5.4s, v25.s[0]                 // vh[1]  = broadcast(vc[1][0]);
        fadd            v5.4s, v5.4s, v21.4s            // vh[1] += vtmp[1];
        fadd            v5.4s, v5.4s, v22.4s            // vh[1] += vtmp[2];
        fadd            v5.4s, v5.4s, v23.4s            // vh[1] += vtmp[3];
        fmul            v21.4s, v16.4s, v26.s[1]        // vtmp[1] = vsrc[0] * vc[2][1];
        fmul            v22.4s, v17.4s, v26.s[2]        // vtmp[2] = vsrc[1] * vc[2][2];
        fmul            v23.4s, v18.4s, v26.s[3]        // vtmp[3] = vsrc[2] * vc[2][3];
        dup             v6.4s, v26.s[0]                 // vh[2]  = broadcast(vc[2][0]);
        fadd            v6.4s, v6.4s, v21.4s            // vh[2] += vtmp[1];
        fadd            v6.4s, v6.4s, v22.4s            // vh[2] += vtmp[2];
        fadd            v6.4s, v6.4s, v23.4s            // vh[2] += vtmp[3];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_000ff3fcff_1_8_f32_0111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s, v25.4s, v26.4s}, [x16] // coeff_veclist = *vcoeff_ptr;
        mov             v16.16b, v0.16b                 // vsrc[0] = vl[0];
        mov             v17.16b, v1.16b                 // vsrc[1] = vl[1];
        mov             v18.16b, v2.16b                 // vsrc[2] = vl[2];
        dup             v0.4s, v24.s[0]                 // vl[0]  = broadcast(vc[0][0]);
        fmla            v0.4s, v16.4s, v24.s[1]         // vl[0] += vsrc[0] * vc[0][1];
        fmla            v0.4s, v17.4s, v24.s[2]         // vl[0] += vsrc[1] * vc[0][2];
        fmla            v0.4s, v18.4s, v24.s[3]         // vl[0] += vsrc[2] * vc[0][3];
        dup             v1.4s, v25.s[0]                 // vl[1]  = broadcast(vc[1][0]);
        fmla            v1.4s, v16.4s, v25.s[1]         // vl[1] += vsrc[0] * vc[1][1];
        fmla            v1.4s, v17.4s, v25.s[2]         // vl[1] += vsrc[1] * vc[1][2];
        fmla            v1.4s, v18.4s, v25.s[3]         // vl[1] += vsrc[2] * vc[1][3];
        dup             v2.4s, v26.s[0]                 // vl[2]  = broadcast(vc[2][0]);
        fmla            v2.4s, v16.4s, v26.s[1]         // vl[2] += vsrc[0] * vc[2][1];
        fmla            v2.4s, v17.4s, v26.s[2]         // vl[2] += vsrc[1] * vc[2][2];
        fmla            v2.4s, v18.4s, v26.s[3]         // vl[2] += vsrc[2] * vc[2][3];
        mov             v16.16b, v4.16b                 // vsrc[0] = vh[0];
        mov             v17.16b, v5.16b                 // vsrc[1] = vh[1];
        mov             v18.16b, v6.16b                 // vsrc[2] = vh[2];
        dup             v4.4s, v24.s[0]                 // vh[0]  = broadcast(vc[0][0]);
        fmla            v4.4s, v16.4s, v24.s[1]         // vh[0] += vsrc[0] * vc[0][1];
        fmla            v4.4s, v17.4s, v24.s[2]         // vh[0] += vsrc[1] * vc[0][2];
        fmla            v4.4s, v18.4s, v24.s[3]         // vh[0] += vsrc[2] * vc[0][3];
        dup             v5.4s, v25.s[0]                 // vh[1]  = broadcast(vc[1][0]);
        fmla            v5.4s, v16.4s, v25.s[1]         // vh[1] += vsrc[0] * vc[1][1];
        fmla            v5.4s, v17.4s, v25.s[2]         // vh[1] += vsrc[1] * vc[1][2];
        fmla            v5.4s, v18.4s, v25.s[3]         // vh[1] += vsrc[2] * vc[1][3];
        dup             v6.4s, v26.s[0]                 // vh[2]  = broadcast(vc[2][0]);
        fmla            v6.4s, v16.4s, v26.s[1]         // vh[2] += vsrc[0] * vc[2][1];
        fmla            v6.4s, v17.4s, v26.s[2]         // vh[2] += vsrc[1] * vc[2][2];
        fmla            v6.4s, v18.4s, v26.s[3]         // vh[2] += vsrc[2] * vc[2][3];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_c000000000_0_8_f32_1000_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s}, [x16]                 // coeff_veclist = *vcoeff_ptr;
        fmul            v3.4s, v3.4s, v24.s[0]          // vl[3]  = vsrc[3] * vc[0][0];
        fmul            v7.4s, v7.4s, v24.s[0]          // vh[3]  = vsrc[3] * vc[0][0];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_c000000000_1_8_f32_1000_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s}, [x16]                 // coeff_veclist = *vcoeff_ptr;
        fmul            v3.4s, v3.4s, v24.s[0]          // vl[3]  = vsrc[3] * vc[0][0];
        fmul            v7.4s, v7.4s, v24.s[0]          // vh[3]  = vsrc[3] * vc[0][0];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_c00000000f_0_8_f32_1001_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s}, [x16]                 // coeff_veclist = *vcoeff_ptr;
        mov             v16.16b, v0.16b                 // vsrc[0] = vl[0];
        fmul            v21.4s, v16.4s, v24.s[1]        // vtmp[1] = vsrc[0] * vc[0][1];
        dup             v0.4s, v24.s[0]                 // vl[0]  = broadcast(vc[0][0]);
        fadd            v0.4s, v0.4s, v21.4s            // vl[0] += vtmp[1];
        fmul            v3.4s, v3.4s, v24.s[2]          // vl[3]  = vsrc[3] * vc[0][2];
        mov             v16.16b, v4.16b                 // vsrc[0] = vh[0];
        fmul            v21.4s, v16.4s, v24.s[1]        // vtmp[1] = vsrc[0] * vc[0][1];
        dup             v4.4s, v24.s[0]                 // vh[0]  = broadcast(vc[0][0]);
        fadd            v4.4s, v4.4s, v21.4s            // vh[0] += vtmp[1];
        fmul            v7.4s, v7.4s, v24.s[2]          // vh[3]  = vsrc[3] * vc[0][2];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_c00000000f_1_8_f32_1001_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s}, [x16]                 // coeff_veclist = *vcoeff_ptr;
        mov             v16.16b, v0.16b                 // vsrc[0] = vl[0];
        dup             v0.4s, v24.s[0]                 // vl[0]  = broadcast(vc[0][0]);
        fmla            v0.4s, v16.4s, v24.s[1]         // vl[0] += vsrc[0] * vc[0][1];
        fmul            v3.4s, v3.4s, v24.s[2]          // vl[3]  = vsrc[3] * vc[0][2];
        mov             v16.16b, v4.16b                 // vsrc[0] = vh[0];
        dup             v4.4s, v24.s[0]                 // vh[0]  = broadcast(vc[0][0]);
        fmla            v4.4s, v16.4s, v24.s[1]         // vh[0] += vsrc[0] * vc[0][1];
        fmul            v7.4s, v7.4s, v24.s[2]          // vh[3]  = vsrc[3] * vc[0][2];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_c0000000fc_0_8_f32_1001_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s}, [x16]                 // coeff_veclist = *vcoeff_ptr;
        fmul            v21.4s, v1.4s, v24.s[1]         // vtmp[1] = vsrc[1] * vc[0][1];
        fmul            v22.4s, v2.4s, v24.s[2]         // vtmp[2] = vsrc[2] * vc[0][2];
        fmul            v0.4s, v0.4s, v24.s[0]          // vl[0]  = vsrc[0] * vc[0][0];
        fadd            v0.4s, v0.4s, v21.4s            // vl[0] += vtmp[1];
        fadd            v0.4s, v0.4s, v22.4s            // vl[0] += vtmp[2];
        fmul            v3.4s, v3.4s, v24.s[3]          // vl[3]  = vsrc[3] * vc[0][3];
        fmul            v21.4s, v5.4s, v24.s[1]         // vtmp[1] = vsrc[1] * vc[0][1];
        fmul            v22.4s, v6.4s, v24.s[2]         // vtmp[2] = vsrc[2] * vc[0][2];
        fmul            v4.4s, v4.4s, v24.s[0]          // vh[0]  = vsrc[0] * vc[0][0];
        fadd            v4.4s, v4.4s, v21.4s            // vh[0] += vtmp[1];
        fadd            v4.4s, v4.4s, v22.4s            // vh[0] += vtmp[2];
        fmul            v7.4s, v7.4s, v24.s[3]          // vh[3]  = vsrc[3] * vc[0][3];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_c0000000fc_1_8_f32_1001_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s}, [x16]                 // coeff_veclist = *vcoeff_ptr;
        fmul            v0.4s, v0.4s, v24.s[0]          // vl[0]  = vsrc[0] * vc[0][0];
        fmla            v0.4s, v1.4s, v24.s[1]          // vl[0] += vsrc[1] * vc[0][1];
        fmla            v0.4s, v2.4s, v24.s[2]          // vl[0] += vsrc[2] * vc[0][2];
        fmul            v3.4s, v3.4s, v24.s[3]          // vl[3]  = vsrc[3] * vc[0][3];
        fmul            v4.4s, v4.4s, v24.s[0]          // vh[0]  = vsrc[0] * vc[0][0];
        fmla            v4.4s, v5.4s, v24.s[1]          // vh[0] += vsrc[1] * vc[0][1];
        fmla            v4.4s, v6.4s, v24.s[2]          // vh[0] += vsrc[2] * vc[0][2];
        fmul            v7.4s, v7.4s, v24.s[3]          // vh[3]  = vsrc[3] * vc[0][3];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_c003f3fccf_0_8_f32_1111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s, v25.4s, v26.4s}, [x16] // coeff_veclist = *vcoeff_ptr;
        mov             v16.16b, v0.16b                 // vsrc[0] = vl[0];
        mov             v17.16b, v1.16b                 // vsrc[1] = vl[1];
        fmul            v21.4s, v16.4s, v24.s[1]        // vtmp[1] = vsrc[0] * vc[0][1];
        fmul            v22.4s, v2.4s, v24.s[2]         // vtmp[2] = vsrc[2] * vc[0][2];
        dup             v0.4s, v24.s[0]                 // vl[0]  = broadcast(vc[0][0]);
        fadd            v0.4s, v0.4s, v21.4s            // vl[0] += vtmp[1];
        fadd            v0.4s, v0.4s, v22.4s            // vl[0] += vtmp[2];
        fmul            v20.4s, v16.4s, v25.s[0]        // vtmp[0] = vsrc[0] * vc[1][0];
        fmul            v21.4s, v17.4s, v25.s[1]        // vtmp[1] = vsrc[1] * vc[1][1];
        fmul            v22.4s, v2.4s, v25.s[2]         // vtmp[2] = vsrc[2] * vc[1][2];
        dup             v1.4s, v24.s[3]                 // vl[1]  = broadcast(vc[0][3]);
        fadd            v1.4s, v1.4s, v20.4s            // vl[1] += vtmp[0];
        fadd            v1.4s, v1.4s, v21.4s            // vl[1] += vtmp[1];
        fadd            v1.4s, v1.4s, v22.4s            // vl[1] += vtmp[2];
        fmul            v20.4s, v16.4s, v26.s[0]        // vtmp[0] = vsrc[0] * vc[2][0];
        fmul            v21.4s, v17.4s, v26.s[1]        // vtmp[1] = vsrc[1] * vc[2][1];
        dup             v2.4s, v25.s[3]                 // vl[2]  = broadcast(vc[1][3]);
        fadd            v2.4s, v2.4s, v20.4s            // vl[2] += vtmp[0];
        fadd            v2.4s, v2.4s, v21.4s            // vl[2] += vtmp[1];
        fmul            v3.4s, v3.4s, v26.s[2]          // vl[3]  = vsrc[3] * vc[2][2];
        mov             v16.16b, v4.16b                 // vsrc[0] = vh[0];
        mov             v17.16b, v5.16b                 // vsrc[1] = vh[1];
        fmul            v21.4s, v16.4s, v24.s[1]        // vtmp[1] = vsrc[0] * vc[0][1];
        fmul            v22.4s, v6.4s, v24.s[2]         // vtmp[2] = vsrc[2] * vc[0][2];
        dup             v4.4s, v24.s[0]                 // vh[0]  = broadcast(vc[0][0]);
        fadd            v4.4s, v4.4s, v21.4s            // vh[0] += vtmp[1];
        fadd            v4.4s, v4.4s, v22.4s            // vh[0] += vtmp[2];
        fmul            v20.4s, v16.4s, v25.s[0]        // vtmp[0] = vsrc[0] * vc[1][0];
        fmul            v21.4s, v17.4s, v25.s[1]        // vtmp[1] = vsrc[1] * vc[1][1];
        fmul            v22.4s, v6.4s, v25.s[2]         // vtmp[2] = vsrc[2] * vc[1][2];
        dup             v5.4s, v24.s[3]                 // vh[1]  = broadcast(vc[0][3]);
        fadd            v5.4s, v5.4s, v20.4s            // vh[1] += vtmp[0];
        fadd            v5.4s, v5.4s, v21.4s            // vh[1] += vtmp[1];
        fadd            v5.4s, v5.4s, v22.4s            // vh[1] += vtmp[2];
        fmul            v20.4s, v16.4s, v26.s[0]        // vtmp[0] = vsrc[0] * vc[2][0];
        fmul            v21.4s, v17.4s, v26.s[1]        // vtmp[1] = vsrc[1] * vc[2][1];
        dup             v6.4s, v25.s[3]                 // vh[2]  = broadcast(vc[1][3]);
        fadd            v6.4s, v6.4s, v20.4s            // vh[2] += vtmp[0];
        fadd            v6.4s, v6.4s, v21.4s            // vh[2] += vtmp[1];
        fmul            v7.4s, v7.4s, v26.s[2]          // vh[3]  = vsrc[3] * vc[2][2];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_c003f3fccf_1_8_f32_1111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s, v25.4s, v26.4s}, [x16] // coeff_veclist = *vcoeff_ptr;
        mov             v16.16b, v0.16b                 // vsrc[0] = vl[0];
        mov             v17.16b, v1.16b                 // vsrc[1] = vl[1];
        dup             v0.4s, v24.s[0]                 // vl[0]  = broadcast(vc[0][0]);
        fmla            v0.4s, v16.4s, v24.s[1]         // vl[0] += vsrc[0] * vc[0][1];
        fmla            v0.4s, v2.4s, v24.s[2]          // vl[0] += vsrc[2] * vc[0][2];
        dup             v1.4s, v24.s[3]                 // vl[1]  = broadcast(vc[0][3]);
        fmla            v1.4s, v16.4s, v25.s[0]         // vl[1] += vsrc[0] * vc[1][0];
        fmla            v1.4s, v17.4s, v25.s[1]         // vl[1] += vsrc[1] * vc[1][1];
        fmla            v1.4s, v2.4s, v25.s[2]          // vl[1] += vsrc[2] * vc[1][2];
        dup             v2.4s, v25.s[3]                 // vl[2]  = broadcast(vc[1][3]);
        fmla            v2.4s, v16.4s, v26.s[0]         // vl[2] += vsrc[0] * vc[2][0];
        fmla            v2.4s, v17.4s, v26.s[1]         // vl[2] += vsrc[1] * vc[2][1];
        fmul            v3.4s, v3.4s, v26.s[2]          // vl[3]  = vsrc[3] * vc[2][2];
        mov             v16.16b, v4.16b                 // vsrc[0] = vh[0];
        mov             v17.16b, v5.16b                 // vsrc[1] = vh[1];
        dup             v4.4s, v24.s[0]                 // vh[0]  = broadcast(vc[0][0]);
        fmla            v4.4s, v16.4s, v24.s[1]         // vh[0] += vsrc[0] * vc[0][1];
        fmla            v4.4s, v6.4s, v24.s[2]          // vh[0] += vsrc[2] * vc[0][2];
        dup             v5.4s, v24.s[3]                 // vh[1]  = broadcast(vc[0][3]);
        fmla            v5.4s, v16.4s, v25.s[0]         // vh[1] += vsrc[0] * vc[1][0];
        fmla            v5.4s, v17.4s, v25.s[1]         // vh[1] += vsrc[1] * vc[1][1];
        fmla            v5.4s, v6.4s, v25.s[2]          // vh[1] += vsrc[2] * vc[1][2];
        dup             v6.4s, v25.s[3]                 // vh[2]  = broadcast(vc[1][3]);
        fmla            v6.4s, v16.4s, v26.s[0]         // vh[2] += vsrc[0] * vc[2][0];
        fmla            v6.4s, v17.4s, v26.s[1]         // vh[2] += vsrc[1] * vc[2][1];
        fmul            v7.4s, v7.4s, v26.s[2]          // vh[3]  = vsrc[3] * vc[2][2];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_c00c00c00c_0_8_f32_1111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s}, [x16]                 // coeff_veclist = *vcoeff_ptr;
        fmul            v0.4s, v0.4s, v24.s[0]          // vl[0]  = vsrc[0] * vc[0][0];
        fmul            v1.4s, v1.4s, v24.s[1]          // vl[1]  = vsrc[1] * vc[0][1];
        fmul            v2.4s, v2.4s, v24.s[2]          // vl[2]  = vsrc[2] * vc[0][2];
        fmul            v3.4s, v3.4s, v24.s[3]          // vl[3]  = vsrc[3] * vc[0][3];
        fmul            v4.4s, v4.4s, v24.s[0]          // vh[0]  = vsrc[0] * vc[0][0];
        fmul            v5.4s, v5.4s, v24.s[1]          // vh[1]  = vsrc[1] * vc[0][1];
        fmul            v6.4s, v6.4s, v24.s[2]          // vh[2]  = vsrc[2] * vc[0][2];
        fmul            v7.4s, v7.4s, v24.s[3]          // vh[3]  = vsrc[3] * vc[0][3];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_c00c00c00c_1_8_f32_1111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s}, [x16]                 // coeff_veclist = *vcoeff_ptr;
        fmul            v0.4s, v0.4s, v24.s[0]          // vl[0]  = vsrc[0] * vc[0][0];
        fmul            v1.4s, v1.4s, v24.s[1]          // vl[1]  = vsrc[1] * vc[0][1];
        fmul            v2.4s, v2.4s, v24.s[2]          // vl[2]  = vsrc[2] * vc[0][2];
        fmul            v3.4s, v3.4s, v24.s[3]          // vl[3]  = vsrc[3] * vc[0][3];
        fmul            v4.4s, v4.4s, v24.s[0]          // vh[0]  = vsrc[0] * vc[0][0];
        fmul            v5.4s, v5.4s, v24.s[1]          // vh[1]  = vsrc[1] * vc[0][1];
        fmul            v6.4s, v6.4s, v24.s[2]          // vh[2]  = vsrc[2] * vc[0][2];
        fmul            v7.4s, v7.4s, v24.s[3]          // vh[3]  = vsrc[3] * vc[0][3];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_c00ff3fcff_0_8_f32_1111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s, v25.4s, v26.4s, v27.4s}, [x16] // coeff_veclist = *vcoeff_ptr;
        mov             v16.16b, v0.16b                 // vsrc[0] = vl[0];
        mov             v17.16b, v1.16b                 // vsrc[1] = vl[1];
        mov             v18.16b, v2.16b                 // vsrc[2] = vl[2];
        fmul            v21.4s, v16.4s, v24.s[1]        // vtmp[1] = vsrc[0] * vc[0][1];
        fmul            v22.4s, v17.4s, v24.s[2]        // vtmp[2] = vsrc[1] * vc[0][2];
        fmul            v23.4s, v18.4s, v24.s[3]        // vtmp[3] = vsrc[2] * vc[0][3];
        dup             v0.4s, v24.s[0]                 // vl[0]  = broadcast(vc[0][0]);
        fadd            v0.4s, v0.4s, v21.4s            // vl[0] += vtmp[1];
        fadd            v0.4s, v0.4s, v22.4s            // vl[0] += vtmp[2];
        fadd            v0.4s, v0.4s, v23.4s            // vl[0] += vtmp[3];
        fmul            v21.4s, v16.4s, v25.s[1]        // vtmp[1] = vsrc[0] * vc[1][1];
        fmul            v22.4s, v17.4s, v25.s[2]        // vtmp[2] = vsrc[1] * vc[1][2];
        fmul            v23.4s, v18.4s, v25.s[3]        // vtmp[3] = vsrc[2] * vc[1][3];
        dup             v1.4s, v25.s[0]                 // vl[1]  = broadcast(vc[1][0]);
        fadd            v1.4s, v1.4s, v21.4s            // vl[1] += vtmp[1];
        fadd            v1.4s, v1.4s, v22.4s            // vl[1] += vtmp[2];
        fadd            v1.4s, v1.4s, v23.4s            // vl[1] += vtmp[3];
        fmul            v21.4s, v16.4s, v26.s[1]        // vtmp[1] = vsrc[0] * vc[2][1];
        fmul            v22.4s, v17.4s, v26.s[2]        // vtmp[2] = vsrc[1] * vc[2][2];
        fmul            v23.4s, v18.4s, v26.s[3]        // vtmp[3] = vsrc[2] * vc[2][3];
        dup             v2.4s, v26.s[0]                 // vl[2]  = broadcast(vc[2][0]);
        fadd            v2.4s, v2.4s, v21.4s            // vl[2] += vtmp[1];
        fadd            v2.4s, v2.4s, v22.4s            // vl[2] += vtmp[2];
        fadd            v2.4s, v2.4s, v23.4s            // vl[2] += vtmp[3];
        fmul            v3.4s, v3.4s, v27.s[0]          // vl[3]  = vsrc[3] * vc[3][0];
        mov             v16.16b, v4.16b                 // vsrc[0] = vh[0];
        mov             v17.16b, v5.16b                 // vsrc[1] = vh[1];
        mov             v18.16b, v6.16b                 // vsrc[2] = vh[2];
        fmul            v21.4s, v16.4s, v24.s[1]        // vtmp[1] = vsrc[0] * vc[0][1];
        fmul            v22.4s, v17.4s, v24.s[2]        // vtmp[2] = vsrc[1] * vc[0][2];
        fmul            v23.4s, v18.4s, v24.s[3]        // vtmp[3] = vsrc[2] * vc[0][3];
        dup             v4.4s, v24.s[0]                 // vh[0]  = broadcast(vc[0][0]);
        fadd            v4.4s, v4.4s, v21.4s            // vh[0] += vtmp[1];
        fadd            v4.4s, v4.4s, v22.4s            // vh[0] += vtmp[2];
        fadd            v4.4s, v4.4s, v23.4s            // vh[0] += vtmp[3];
        fmul            v21.4s, v16.4s, v25.s[1]        // vtmp[1] = vsrc[0] * vc[1][1];
        fmul            v22.4s, v17.4s, v25.s[2]        // vtmp[2] = vsrc[1] * vc[1][2];
        fmul            v23.4s, v18.4s, v25.s[3]        // vtmp[3] = vsrc[2] * vc[1][3];
        dup             v5.4s, v25.s[0]                 // vh[1]  = broadcast(vc[1][0]);
        fadd            v5.4s, v5.4s, v21.4s            // vh[1] += vtmp[1];
        fadd            v5.4s, v5.4s, v22.4s            // vh[1] += vtmp[2];
        fadd            v5.4s, v5.4s, v23.4s            // vh[1] += vtmp[3];
        fmul            v21.4s, v16.4s, v26.s[1]        // vtmp[1] = vsrc[0] * vc[2][1];
        fmul            v22.4s, v17.4s, v26.s[2]        // vtmp[2] = vsrc[1] * vc[2][2];
        fmul            v23.4s, v18.4s, v26.s[3]        // vtmp[3] = vsrc[2] * vc[2][3];
        dup             v6.4s, v26.s[0]                 // vh[2]  = broadcast(vc[2][0]);
        fadd            v6.4s, v6.4s, v21.4s            // vh[2] += vtmp[1];
        fadd            v6.4s, v6.4s, v22.4s            // vh[2] += vtmp[2];
        fadd            v6.4s, v6.4s, v23.4s            // vh[2] += vtmp[3];
        fmul            v7.4s, v7.4s, v27.s[0]          // vh[3]  = vsrc[3] * vc[3][0];
        br              x0                              // jump to cont
endfunc

function ff_sws_linear_c00ff3fcff_1_8_f32_1111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // v128 *vcoeff_ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ld1             {v24.4s, v25.4s, v26.4s, v27.4s}, [x16] // coeff_veclist = *vcoeff_ptr;
        mov             v16.16b, v0.16b                 // vsrc[0] = vl[0];
        mov             v17.16b, v1.16b                 // vsrc[1] = vl[1];
        mov             v18.16b, v2.16b                 // vsrc[2] = vl[2];
        dup             v0.4s, v24.s[0]                 // vl[0]  = broadcast(vc[0][0]);
        fmla            v0.4s, v16.4s, v24.s[1]         // vl[0] += vsrc[0] * vc[0][1];
        fmla            v0.4s, v17.4s, v24.s[2]         // vl[0] += vsrc[1] * vc[0][2];
        fmla            v0.4s, v18.4s, v24.s[3]         // vl[0] += vsrc[2] * vc[0][3];
        dup             v1.4s, v25.s[0]                 // vl[1]  = broadcast(vc[1][0]);
        fmla            v1.4s, v16.4s, v25.s[1]         // vl[1] += vsrc[0] * vc[1][1];
        fmla            v1.4s, v17.4s, v25.s[2]         // vl[1] += vsrc[1] * vc[1][2];
        fmla            v1.4s, v18.4s, v25.s[3]         // vl[1] += vsrc[2] * vc[1][3];
        dup             v2.4s, v26.s[0]                 // vl[2]  = broadcast(vc[2][0]);
        fmla            v2.4s, v16.4s, v26.s[1]         // vl[2] += vsrc[0] * vc[2][1];
        fmla            v2.4s, v17.4s, v26.s[2]         // vl[2] += vsrc[1] * vc[2][2];
        fmla            v2.4s, v18.4s, v26.s[3]         // vl[2] += vsrc[2] * vc[2][3];
        fmul            v3.4s, v3.4s, v27.s[0]          // vl[3]  = vsrc[3] * vc[3][0];
        mov             v16.16b, v4.16b                 // vsrc[0] = vh[0];
        mov             v17.16b, v5.16b                 // vsrc[1] = vh[1];
        mov             v18.16b, v6.16b                 // vsrc[2] = vh[2];
        dup             v4.4s, v24.s[0]                 // vh[0]  = broadcast(vc[0][0]);
        fmla            v4.4s, v16.4s, v24.s[1]         // vh[0] += vsrc[0] * vc[0][1];
        fmla            v4.4s, v17.4s, v24.s[2]         // vh[0] += vsrc[1] * vc[0][2];
        fmla            v4.4s, v18.4s, v24.s[3]         // vh[0] += vsrc[2] * vc[0][3];
        dup             v5.4s, v25.s[0]                 // vh[1]  = broadcast(vc[1][0]);
        fmla            v5.4s, v16.4s, v25.s[1]         // vh[1] += vsrc[0] * vc[1][1];
        fmla            v5.4s, v17.4s, v25.s[2]         // vh[1] += vsrc[1] * vc[1][2];
        fmla            v5.4s, v18.4s, v25.s[3]         // vh[1] += vsrc[2] * vc[1][3];
        dup             v6.4s, v26.s[0]                 // vh[2]  = broadcast(vc[2][0]);
        fmla            v6.4s, v16.4s, v26.s[1]         // vh[2] += vsrc[0] * vc[2][1];
        fmla            v6.4s, v17.4s, v26.s[2]         // vh[2] += vsrc[1] * vc[2][2];
        fmla            v6.4s, v18.4s, v26.s[3]         // vh[2] += vsrc[2] * vc[2][3];
        fmul            v7.4s, v7.4s, v27.s[0]          // vh[3]  = vsrc[3] * vc[3][0];
        br              x0                              // jump to cont
endfunc

function ff_sws_dither_0325_4_8_f32_1111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // void *ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ubfiz           x17, x6, #5, #1                 // tmp1 = (bx & ((dither_size / block_size) - 1)) * block_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ubfiz           x17, x3, #6, #4                 // tmp1 = (y & (dither_size - 1)) * dither_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v3.4s, v3.4s, v16.4s            // vl[3] += vditherl;
        fadd            v7.4s, v7.4s, v17.4s            // vh[3] += vditherh;
        add             x16, x16, #128                  // ptr += (y_off[1] - y_off[3]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v1.4s, v1.4s, v16.4s            // vl[1] += vditherl;
        fadd            v5.4s, v5.4s, v17.4s            // vh[1] += vditherh;
        add             x16, x16, #64                   // ptr += (y_off[2] - y_off[1]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v2.4s, v2.4s, v16.4s            // vl[2] += vditherl;
        fadd            v6.4s, v6.4s, v17.4s            // vh[2] += vditherh;
        add             x16, x16, #128                  // ptr += (y_off[0] - y_off[2]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v0.4s, v0.4s, v16.4s            // vl[0] += vditherl;
        fadd            v4.4s, v4.4s, v17.4s            // vh[0] += vditherh;
        br              x0                              // jump to cont
endfunc

function ff_sws_dither_032f_4_8_f32_1110_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // void *ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ubfiz           x17, x6, #5, #1                 // tmp1 = (bx & ((dither_size / block_size) - 1)) * block_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ubfiz           x17, x3, #6, #4                 // tmp1 = (y & (dither_size - 1)) * dither_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v3.4s, v3.4s, v16.4s            // vl[3] += vditherl;
        fadd            v7.4s, v7.4s, v17.4s            // vh[3] += vditherh;
        add             x16, x16, #128                  // ptr += (y_off[1] - y_off[3]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v1.4s, v1.4s, v16.4s            // vl[1] += vditherl;
        fadd            v5.4s, v5.4s, v17.4s            // vh[1] += vditherh;
        add             x16, x16, #64                   // ptr += (y_off[2] - y_off[1]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v2.4s, v2.4s, v16.4s            // vl[2] += vditherl;
        fadd            v6.4s, v6.4s, v17.4s            // vh[2] += vditherh;
        br              x0                              // jump to cont
endfunc

function ff_sws_dither_2305_4_8_f32_1111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // void *ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ubfiz           x17, x6, #5, #1                 // tmp1 = (bx & ((dither_size / block_size) - 1)) * block_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ubfiz           x17, x3, #6, #4                 // tmp1 = (y & (dither_size - 1)) * dither_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v1.4s, v1.4s, v16.4s            // vl[1] += vditherl;
        fadd            v5.4s, v5.4s, v17.4s            // vh[1] += vditherh;
        add             x16, x16, #128                  // ptr += (y_off[3] - y_off[1]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v3.4s, v3.4s, v16.4s            // vl[3] += vditherl;
        fadd            v7.4s, v7.4s, v17.4s            // vh[3] += vditherh;
        add             x16, x16, #64                   // ptr += (y_off[2] - y_off[3]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v2.4s, v2.4s, v16.4s            // vl[2] += vditherl;
        fadd            v6.4s, v6.4s, v17.4s            // vh[2] += vditherh;
        add             x16, x16, #128                  // ptr += (y_off[0] - y_off[2]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v0.4s, v0.4s, v16.4s            // vl[0] += vditherl;
        fadd            v4.4s, v4.4s, v17.4s            // vh[0] += vditherh;
        br              x0                              // jump to cont
endfunc

function ff_sws_dither_230f_4_8_f32_1110_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // void *ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ubfiz           x17, x6, #5, #1                 // tmp1 = (bx & ((dither_size / block_size) - 1)) * block_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ubfiz           x17, x3, #6, #4                 // tmp1 = (y & (dither_size - 1)) * dither_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v1.4s, v1.4s, v16.4s            // vl[1] += vditherl;
        fadd            v5.4s, v5.4s, v17.4s            // vh[1] += vditherh;
        add             x16, x16, #128                  // ptr += (y_off[3] - y_off[1]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v3.4s, v3.4s, v16.4s            // vl[3] += vditherl;
        fadd            v7.4s, v7.4s, v17.4s            // vh[3] += vditherh;
        add             x16, x16, #64                   // ptr += (y_off[2] - y_off[3]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v2.4s, v2.4s, v16.4s            // vl[2] += vditherl;
        fadd            v6.4s, v6.4s, v17.4s            // vh[2] += vditherh;
        br              x0                              // jump to cont
endfunc

function ff_sws_dither_3000_4_8_f32_1111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // void *ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ubfiz           x17, x6, #5, #1                 // tmp1 = (bx & ((dither_size / block_size) - 1)) * block_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ubfiz           x17, x3, #6, #4                 // tmp1 = (y & (dither_size - 1)) * dither_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v0.4s, v0.4s, v16.4s            // vl[0] += vditherl;
        fadd            v4.4s, v4.4s, v17.4s            // vh[0] += vditherh;
        fadd            v1.4s, v1.4s, v16.4s            // vl[1] += vditherl;
        fadd            v5.4s, v5.4s, v17.4s            // vh[1] += vditherh;
        fadd            v2.4s, v2.4s, v16.4s            // vl[2] += vditherl;
        fadd            v6.4s, v6.4s, v17.4s            // vh[2] += vditherh;
        add             x16, x16, #192                  // ptr += (y_off[3] - y_off[2]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v3.4s, v3.4s, v16.4s            // vl[3] += vditherl;
        fadd            v7.4s, v7.4s, v17.4s            // vh[3] += vditherh;
        br              x0                              // jump to cont
endfunc

function ff_sws_dither_302f_4_8_f32_1110_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // void *ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ubfiz           x17, x6, #5, #1                 // tmp1 = (bx & ((dither_size / block_size) - 1)) * block_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ubfiz           x17, x3, #6, #4                 // tmp1 = (y & (dither_size - 1)) * dither_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v2.4s, v2.4s, v16.4s            // vl[2] += vditherl;
        fadd            v6.4s, v6.4s, v17.4s            // vh[2] += vditherh;
        add             x16, x16, #128                  // ptr += (y_off[1] - y_off[2]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v1.4s, v1.4s, v16.4s            // vl[1] += vditherl;
        fadd            v5.4s, v5.4s, v17.4s            // vh[1] += vditherh;
        add             x16, x16, #64                   // ptr += (y_off[3] - y_off[1]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v3.4s, v3.4s, v16.4s            // vl[3] += vditherl;
        fadd            v7.4s, v7.4s, v17.4s            // vh[3] += vditherh;
        br              x0                              // jump to cont
endfunc

function ff_sws_dither_3ff0_4_8_f32_1001_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // void *ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ubfiz           x17, x6, #5, #1                 // tmp1 = (bx & ((dither_size / block_size) - 1)) * block_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ubfiz           x17, x3, #6, #4                 // tmp1 = (y & (dither_size - 1)) * dither_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v0.4s, v0.4s, v16.4s            // vl[0] += vditherl;
        fadd            v4.4s, v4.4s, v17.4s            // vh[0] += vditherh;
        add             x16, x16, #192                  // ptr += (y_off[3] - y_off[0]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v3.4s, v3.4s, v16.4s            // vl[3] += vditherl;
        fadd            v7.4s, v7.4s, v17.4s            // vh[3] += vditherh;
        br              x0                              // jump to cont
endfunc

function ff_sws_dither_5023_4_8_f32_1111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // void *ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ubfiz           x17, x6, #5, #1                 // tmp1 = (bx & ((dither_size / block_size) - 1)) * block_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ubfiz           x17, x3, #6, #4                 // tmp1 = (y & (dither_size - 1)) * dither_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v2.4s, v2.4s, v16.4s            // vl[2] += vditherl;
        fadd            v6.4s, v6.4s, v17.4s            // vh[2] += vditherh;
        add             x16, x16, #128                  // ptr += (y_off[1] - y_off[2]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v1.4s, v1.4s, v16.4s            // vl[1] += vditherl;
        fadd            v5.4s, v5.4s, v17.4s            // vh[1] += vditherh;
        add             x16, x16, #64                   // ptr += (y_off[0] - y_off[1]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v0.4s, v0.4s, v16.4s            // vl[0] += vditherl;
        fadd            v4.4s, v4.4s, v17.4s            // vh[0] += vditherh;
        add             x16, x16, #128                  // ptr += (y_off[3] - y_off[0]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v3.4s, v3.4s, v16.4s            // vl[3] += vditherl;
        fadd            v7.4s, v7.4s, v17.4s            // vh[3] += vditherh;
        br              x0                              // jump to cont
endfunc

function ff_sws_dither_5032_4_8_f32_1111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // void *ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ubfiz           x17, x6, #5, #1                 // tmp1 = (bx & ((dither_size / block_size) - 1)) * block_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ubfiz           x17, x3, #6, #4                 // tmp1 = (y & (dither_size - 1)) * dither_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v2.4s, v2.4s, v16.4s            // vl[2] += vditherl;
        fadd            v6.4s, v6.4s, v17.4s            // vh[2] += vditherh;
        add             x16, x16, #128                  // ptr += (y_off[0] - y_off[2]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v0.4s, v0.4s, v16.4s            // vl[0] += vditherl;
        fadd            v4.4s, v4.4s, v17.4s            // vh[0] += vditherh;
        add             x16, x16, #64                   // ptr += (y_off[1] - y_off[0]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v1.4s, v1.4s, v16.4s            // vl[1] += vditherl;
        fadd            v5.4s, v5.4s, v17.4s            // vh[1] += vditherh;
        add             x16, x16, #128                  // ptr += (y_off[3] - y_off[1]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v3.4s, v3.4s, v16.4s            // vl[3] += vditherl;
        fadd            v7.4s, v7.4s, v17.4s            // vh[3] += vditherh;
        br              x0                              // jump to cont
endfunc

function ff_sws_dither_5230_4_8_f32_1111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // void *ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ubfiz           x17, x6, #5, #1                 // tmp1 = (bx & ((dither_size / block_size) - 1)) * block_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ubfiz           x17, x3, #6, #4                 // tmp1 = (y & (dither_size - 1)) * dither_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v0.4s, v0.4s, v16.4s            // vl[0] += vditherl;
        fadd            v4.4s, v4.4s, v17.4s            // vh[0] += vditherh;
        add             x16, x16, #128                  // ptr += (y_off[2] - y_off[0]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v2.4s, v2.4s, v16.4s            // vl[2] += vditherl;
        fadd            v6.4s, v6.4s, v17.4s            // vh[2] += vditherh;
        add             x16, x16, #64                   // ptr += (y_off[1] - y_off[2]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v1.4s, v1.4s, v16.4s            // vl[1] += vditherl;
        fadd            v5.4s, v5.4s, v17.4s            // vh[1] += vditherh;
        add             x16, x16, #128                  // ptr += (y_off[3] - y_off[1]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v3.4s, v3.4s, v16.4s            // vl[3] += vditherl;
        fadd            v7.4s, v7.4s, v17.4s            // vh[3] += vditherh;
        br              x0                              // jump to cont
endfunc

function ff_sws_dither_5ff0_4_8_f32_1001_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // void *ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ubfiz           x17, x6, #5, #1                 // tmp1 = (bx & ((dither_size / block_size) - 1)) * block_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ubfiz           x17, x3, #6, #4                 // tmp1 = (y & (dither_size - 1)) * dither_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v0.4s, v0.4s, v16.4s            // vl[0] += vditherl;
        fadd            v4.4s, v4.4s, v17.4s            // vh[0] += vditherh;
        add             x16, x16, #320                  // ptr += (y_off[3] - y_off[0]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v3.4s, v3.4s, v16.4s            // vl[3] += vditherl;
        fadd            v7.4s, v7.4s, v17.4s            // vh[3] += vditherh;
        br              x0                              // jump to cont
endfunc

function ff_sws_dither_5fff_4_8_f32_1000_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // void *ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ubfiz           x17, x6, #5, #1                 // tmp1 = (bx & ((dither_size / block_size) - 1)) * block_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        add             w17, w3, #5                     // tmp1 = y + y_off[3];
        ubfiz           x17, x17, #6, #4                // tmp1 = (tmp1 & (dither_size - 1)) * dither_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v3.4s, v3.4s, v16.4s            // vl[3] += vditherl;
        fadd            v7.4s, v7.4s, v17.4s            // vh[3] += vditherh;
        br              x0                              // jump to cont
endfunc

function ff_sws_dither_f023_4_8_f32_0111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // void *ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ubfiz           x17, x6, #5, #1                 // tmp1 = (bx & ((dither_size / block_size) - 1)) * block_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ubfiz           x17, x3, #6, #4                 // tmp1 = (y & (dither_size - 1)) * dither_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v2.4s, v2.4s, v16.4s            // vl[2] += vditherl;
        fadd            v6.4s, v6.4s, v17.4s            // vh[2] += vditherh;
        add             x16, x16, #128                  // ptr += (y_off[1] - y_off[2]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v1.4s, v1.4s, v16.4s            // vl[1] += vditherl;
        fadd            v5.4s, v5.4s, v17.4s            // vh[1] += vditherh;
        add             x16, x16, #64                   // ptr += (y_off[0] - y_off[1]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v0.4s, v0.4s, v16.4s            // vl[0] += vditherl;
        fadd            v4.4s, v4.4s, v17.4s            // vh[0] += vditherh;
        br              x0                              // jump to cont
endfunc

function ff_sws_dither_f032_4_8_f32_0111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // void *ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ubfiz           x17, x6, #5, #1                 // tmp1 = (bx & ((dither_size / block_size) - 1)) * block_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ubfiz           x17, x3, #6, #4                 // tmp1 = (y & (dither_size - 1)) * dither_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v2.4s, v2.4s, v16.4s            // vl[2] += vditherl;
        fadd            v6.4s, v6.4s, v17.4s            // vh[2] += vditherh;
        add             x16, x16, #128                  // ptr += (y_off[0] - y_off[2]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v0.4s, v0.4s, v16.4s            // vl[0] += vditherl;
        fadd            v4.4s, v4.4s, v17.4s            // vh[0] += vditherh;
        add             x16, x16, #64                   // ptr += (y_off[1] - y_off[0]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v1.4s, v1.4s, v16.4s            // vl[1] += vditherl;
        fadd            v5.4s, v5.4s, v17.4s            // vh[1] += vditherh;
        br              x0                              // jump to cont
endfunc

function ff_sws_dither_f203_4_8_f32_0111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // void *ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ubfiz           x17, x6, #5, #1                 // tmp1 = (bx & ((dither_size / block_size) - 1)) * block_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ubfiz           x17, x3, #6, #4                 // tmp1 = (y & (dither_size - 1)) * dither_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v1.4s, v1.4s, v16.4s            // vl[1] += vditherl;
        fadd            v5.4s, v5.4s, v17.4s            // vh[1] += vditherh;
        add             x16, x16, #128                  // ptr += (y_off[2] - y_off[1]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v2.4s, v2.4s, v16.4s            // vl[2] += vditherl;
        fadd            v6.4s, v6.4s, v17.4s            // vh[2] += vditherh;
        add             x16, x16, #64                   // ptr += (y_off[0] - y_off[2]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v0.4s, v0.4s, v16.4s            // vl[0] += vditherl;
        fadd            v4.4s, v4.4s, v17.4s            // vh[0] += vditherh;
        br              x0                              // jump to cont
endfunc

function ff_sws_dither_f230_4_8_f32_0111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // void *ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ubfiz           x17, x6, #5, #1                 // tmp1 = (bx & ((dither_size / block_size) - 1)) * block_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ubfiz           x17, x3, #6, #4                 // tmp1 = (y & (dither_size - 1)) * dither_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v0.4s, v0.4s, v16.4s            // vl[0] += vditherl;
        fadd            v4.4s, v4.4s, v17.4s            // vh[0] += vditherh;
        add             x16, x16, #128                  // ptr += (y_off[2] - y_off[0]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v2.4s, v2.4s, v16.4s            // vl[2] += vditherl;
        fadd            v6.4s, v6.4s, v17.4s            // vh[2] += vditherh;
        add             x16, x16, #64                   // ptr += (y_off[1] - y_off[2]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v1.4s, v1.4s, v16.4s            // vl[1] += vditherl;
        fadd            v5.4s, v5.4s, v17.4s            // vh[1] += vditherh;
        br              x0                              // jump to cont
endfunc

function ff_sws_dither_f2f0_4_8_f32_0101_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // void *ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ubfiz           x17, x6, #5, #1                 // tmp1 = (bx & ((dither_size / block_size) - 1)) * block_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ubfiz           x17, x3, #6, #4                 // tmp1 = (y & (dither_size - 1)) * dither_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v0.4s, v0.4s, v16.4s            // vl[0] += vditherl;
        fadd            v4.4s, v4.4s, v17.4s            // vh[0] += vditherh;
        add             x16, x16, #128                  // ptr += (y_off[2] - y_off[0]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v2.4s, v2.4s, v16.4s            // vl[2] += vditherl;
        fadd            v6.4s, v6.4s, v17.4s            // vh[2] += vditherh;
        br              x0                              // jump to cont
endfunc

function ff_sws_dither_f2ff_4_8_f32_0100_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // void *ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ubfiz           x17, x6, #5, #1                 // tmp1 = (bx & ((dither_size / block_size) - 1)) * block_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        add             w17, w3, #2                     // tmp1 = y + y_off[2];
        ubfiz           x17, x17, #6, #4                // tmp1 = (tmp1 & (dither_size - 1)) * dither_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v2.4s, v2.4s, v16.4s            // vl[2] += vditherl;
        fadd            v6.4s, v6.4s, v17.4s            // vh[2] += vditherh;
        br              x0                              // jump to cont
endfunc

function ff_sws_dither_f302_4_8_f32_0111_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // void *ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ubfiz           x17, x6, #5, #1                 // tmp1 = (bx & ((dither_size / block_size) - 1)) * block_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ubfiz           x17, x3, #6, #4                 // tmp1 = (y & (dither_size - 1)) * dither_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v1.4s, v1.4s, v16.4s            // vl[1] += vditherl;
        fadd            v5.4s, v5.4s, v17.4s            // vh[1] += vditherh;
        add             x16, x16, #128                  // ptr += (y_off[0] - y_off[1]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v0.4s, v0.4s, v16.4s            // vl[0] += vditherl;
        fadd            v4.4s, v4.4s, v17.4s            // vh[0] += vditherh;
        add             x16, x16, #64                   // ptr += (y_off[2] - y_off[0]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v2.4s, v2.4s, v16.4s            // vl[2] += vditherl;
        fadd            v6.4s, v6.4s, v17.4s            // vh[2] += vditherh;
        br              x0                              // jump to cont
endfunc

function ff_sws_dither_ff30_4_8_f32_0011_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // void *ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ubfiz           x17, x6, #5, #1                 // tmp1 = (bx & ((dither_size / block_size) - 1)) * block_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ubfiz           x17, x3, #6, #4                 // tmp1 = (y & (dither_size - 1)) * dither_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v0.4s, v0.4s, v16.4s            // vl[0] += vditherl;
        fadd            v4.4s, v4.4s, v17.4s            // vh[0] += vditherh;
        add             x16, x16, #192                  // ptr += (y_off[1] - y_off[0]) * dither_size * sizeof(float);
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v1.4s, v1.4s, v16.4s            // vl[1] += vditherl;
        fadd            v5.4s, v5.4s, v17.4s            // vh[1] += vditherh;
        br              x0                              // jump to cont
endfunc

function ff_sws_dither_ff3f_4_8_f32_0010_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // void *ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ubfiz           x17, x6, #5, #1                 // tmp1 = (bx & ((dither_size / block_size) - 1)) * block_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        add             w17, w3, #3                     // tmp1 = y + y_off[1];
        ubfiz           x17, x17, #6, #4                // tmp1 = (tmp1 & (dither_size - 1)) * dither_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v1.4s, v1.4s, v16.4s            // vl[1] += vditherl;
        fadd            v5.4s, v5.4s, v17.4s            // vh[1] += vditherh;
        br              x0                              // jump to cont
endfunc

function ff_sws_dither_fff0_4_8_f32_0001_neon, export=1, jumpable=1
        ldr             x16, [x1, #16]                  // void *ptr = impl->priv.ptr;
        ldr             x0, [x1], #32                   // SwsFuncPtr cont = (impl++)->cont;
        ubfiz           x17, x6, #5, #1                 // tmp1 = (bx & ((dither_size / block_size) - 1)) * block_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ubfiz           x17, x3, #6, #4                 // tmp1 = (y & (dither_size - 1)) * dither_size * sizeof(float);
        add             x16, x16, x17                   // ptr += tmp1;
        ldp             q16, q17, [x16]                 // { ditherl, ditherh } = *ptr;
        fadd            v0.4s, v0.4s, v16.4s            // vl[0] += vditherl;
        fadd            v4.4s, v4.4s, v17.4s            // vh[0] += vditherh;
        br              x0                              // jump to cont
endfunc

