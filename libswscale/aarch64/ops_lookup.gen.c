#include "libswscale/aarch64/ops_lookup.h"

extern void ff_sws_read_bit_8_u8_0001_neon(void);
extern void ff_sws_read_bit_16_u8_0001_neon(void);
extern void ff_sws_read_nibble_8_u8_0001_neon(void);
extern void ff_sws_read_nibble_16_u8_0001_neon(void);
extern void ff_sws_read_packed_8_u8_0011_neon(void);
extern void ff_sws_read_packed_8_u8_0111_neon(void);
extern void ff_sws_read_packed_8_u8_1111_neon(void);
extern void ff_sws_read_packed_8_u16_0011_neon(void);
extern void ff_sws_read_packed_8_u16_0111_neon(void);
extern void ff_sws_read_packed_8_u16_1111_neon(void);
extern void ff_sws_read_packed_8_u32_0011_neon(void);
extern void ff_sws_read_packed_8_u32_0111_neon(void);
extern void ff_sws_read_packed_8_u32_1111_neon(void);
extern void ff_sws_read_packed_16_u8_0011_neon(void);
extern void ff_sws_read_packed_16_u8_0111_neon(void);
extern void ff_sws_read_packed_16_u8_1111_neon(void);
extern void ff_sws_read_packed_16_u16_0011_neon(void);
extern void ff_sws_read_packed_16_u16_0111_neon(void);
extern void ff_sws_read_packed_16_u16_1111_neon(void);
extern void ff_sws_read_planar_8_u8_0001_neon(void);
extern void ff_sws_read_planar_8_u8_0011_neon(void);
extern void ff_sws_read_planar_8_u8_0111_neon(void);
extern void ff_sws_read_planar_8_u8_1111_neon(void);
extern void ff_sws_read_planar_8_u16_0001_neon(void);
extern void ff_sws_read_planar_8_u16_0011_neon(void);
extern void ff_sws_read_planar_8_u16_0111_neon(void);
extern void ff_sws_read_planar_8_u16_1111_neon(void);
extern void ff_sws_read_planar_8_u32_0001_neon(void);
extern void ff_sws_read_planar_8_u32_0111_neon(void);
extern void ff_sws_read_planar_8_u32_1111_neon(void);
extern void ff_sws_read_planar_16_u8_0001_neon(void);
extern void ff_sws_read_planar_16_u8_0111_neon(void);
extern void ff_sws_read_planar_16_u8_1111_neon(void);
extern void ff_sws_read_planar_16_u16_0001_neon(void);
extern void ff_sws_read_planar_16_u16_0111_neon(void);
extern void ff_sws_read_planar_16_u16_1111_neon(void);
extern void ff_sws_write_bit_8_u8_0001_neon(void);
extern void ff_sws_write_bit_16_u8_0001_neon(void);
extern void ff_sws_write_nibble_8_u8_0001_neon(void);
extern void ff_sws_write_nibble_16_u8_0001_neon(void);
extern void ff_sws_write_packed_8_u8_0011_neon(void);
extern void ff_sws_write_packed_8_u8_0111_neon(void);
extern void ff_sws_write_packed_8_u8_1111_neon(void);
extern void ff_sws_write_packed_8_u16_0011_neon(void);
extern void ff_sws_write_packed_8_u16_0111_neon(void);
extern void ff_sws_write_packed_8_u16_1111_neon(void);
extern void ff_sws_write_packed_8_u32_0011_neon(void);
extern void ff_sws_write_packed_8_u32_0111_neon(void);
extern void ff_sws_write_packed_8_u32_1111_neon(void);
extern void ff_sws_write_packed_16_u8_0011_neon(void);
extern void ff_sws_write_packed_16_u8_0111_neon(void);
extern void ff_sws_write_packed_16_u8_1111_neon(void);
extern void ff_sws_write_packed_16_u16_0011_neon(void);
extern void ff_sws_write_packed_16_u16_0111_neon(void);
extern void ff_sws_write_packed_16_u16_1111_neon(void);
extern void ff_sws_write_planar_8_u8_0001_neon(void);
extern void ff_sws_write_planar_8_u8_0111_neon(void);
extern void ff_sws_write_planar_8_u8_1111_neon(void);
extern void ff_sws_write_planar_8_u16_0001_neon(void);
extern void ff_sws_write_planar_8_u16_0111_neon(void);
extern void ff_sws_write_planar_8_u16_1111_neon(void);
extern void ff_sws_write_planar_8_u32_0001_neon(void);
extern void ff_sws_write_planar_8_u32_0111_neon(void);
extern void ff_sws_write_planar_8_u32_1111_neon(void);
extern void ff_sws_write_planar_16_u8_0001_neon(void);
extern void ff_sws_write_planar_16_u8_0111_neon(void);
extern void ff_sws_write_planar_16_u8_1111_neon(void);
extern void ff_sws_write_planar_16_u16_0001_neon(void);
extern void ff_sws_write_planar_16_u16_0111_neon(void);
extern void ff_sws_write_planar_16_u16_1111_neon(void);
extern void ff_sws_swap_bytes_8_u16_0001_neon(void);
extern void ff_sws_swap_bytes_8_u16_0010_neon(void);
extern void ff_sws_swap_bytes_8_u16_0011_neon(void);
extern void ff_sws_swap_bytes_8_u16_0111_neon(void);
extern void ff_sws_swap_bytes_8_u16_1001_neon(void);
extern void ff_sws_swap_bytes_8_u16_1110_neon(void);
extern void ff_sws_swap_bytes_8_u16_1111_neon(void);
extern void ff_sws_swap_bytes_8_u32_0001_neon(void);
extern void ff_sws_swap_bytes_8_u32_0011_neon(void);
extern void ff_sws_swap_bytes_8_u32_0111_neon(void);
extern void ff_sws_swap_bytes_8_u32_1001_neon(void);
extern void ff_sws_swap_bytes_8_u32_1110_neon(void);
extern void ff_sws_swap_bytes_8_u32_1111_neon(void);
extern void ff_sws_swap_bytes_16_u16_0001_neon(void);
extern void ff_sws_swap_bytes_16_u16_0011_neon(void);
extern void ff_sws_swap_bytes_16_u16_0111_neon(void);
extern void ff_sws_swap_bytes_16_u16_1110_neon(void);
extern void ff_sws_swap_bytes_16_u16_1111_neon(void);
extern void ff_sws_swizzle_0001_8_u8_1111_neon(void);
extern void ff_sws_swizzle_0001_16_u8_1111_neon(void);
extern void ff_sws_swizzle_000f_8_u8_1110_neon(void);
extern void ff_sws_swizzle_000f_16_u8_1110_neon(void);
extern void ff_sws_swizzle_000f_32_u8_1110_neon(void);
extern void ff_sws_swizzle_0123_8_u8_1111_neon(void);
extern void ff_sws_swizzle_0123_16_u8_1111_neon(void);
extern void ff_sws_swizzle_0123_32_u8_1111_neon(void);
extern void ff_sws_swizzle_012f_8_u8_1110_neon(void);
extern void ff_sws_swizzle_012f_16_u8_1110_neon(void);
extern void ff_sws_swizzle_012f_32_u8_1110_neon(void);
extern void ff_sws_swizzle_0321_16_u8_1111_neon(void);
extern void ff_sws_swizzle_0321_32_u8_1111_neon(void);
extern void ff_sws_swizzle_03f2_16_u8_1101_neon(void);
extern void ff_sws_swizzle_0ff1_32_u8_1001_neon(void);
extern void ff_sws_swizzle_0fff_8_u8_1000_neon(void);
extern void ff_sws_swizzle_0fff_16_u8_1000_neon(void);
extern void ff_sws_swizzle_0fff_32_u8_1000_neon(void);
extern void ff_sws_swizzle_100f_8_u8_1110_neon(void);
extern void ff_sws_swizzle_100f_16_u8_1110_neon(void);
extern void ff_sws_swizzle_100f_32_u8_1110_neon(void);
extern void ff_sws_swizzle_1023_8_u8_1111_neon(void);
extern void ff_sws_swizzle_1023_32_u8_1111_neon(void);
extern void ff_sws_swizzle_102f_8_u8_1110_neon(void);
extern void ff_sws_swizzle_102f_32_u8_1110_neon(void);
extern void ff_sws_swizzle_132f_8_u8_1110_neon(void);
extern void ff_sws_swizzle_132f_16_u8_1110_neon(void);
extern void ff_sws_swizzle_1f0f_32_u8_1010_neon(void);
extern void ff_sws_swizzle_1f3f_8_u8_1010_neon(void);
extern void ff_sws_swizzle_1f3f_16_u8_1010_neon(void);
extern void ff_sws_swizzle_1f3f_32_u8_1010_neon(void);
extern void ff_sws_swizzle_1fff_32_u8_1000_neon(void);
extern void ff_sws_swizzle_20f3_8_u8_1101_neon(void);
extern void ff_sws_swizzle_20f3_16_u8_1101_neon(void);
extern void ff_sws_swizzle_20ff_8_u8_1100_neon(void);
extern void ff_sws_swizzle_20ff_32_u8_1100_neon(void);
extern void ff_sws_swizzle_2103_8_u8_1111_neon(void);
extern void ff_sws_swizzle_2103_16_u8_1111_neon(void);
extern void ff_sws_swizzle_2103_32_u8_1111_neon(void);
extern void ff_sws_swizzle_210f_8_u8_1110_neon(void);
extern void ff_sws_swizzle_210f_16_u8_1110_neon(void);
extern void ff_sws_swizzle_210f_32_u8_1110_neon(void);
extern void ff_sws_swizzle_f00f_8_u8_0110_neon(void);
extern void ff_sws_swizzle_f00f_16_u8_0110_neon(void);
extern void ff_sws_swizzle_f00f_32_u8_0110_neon(void);
extern void ff_sws_swizzle_f021_8_u8_0111_neon(void);
extern void ff_sws_swizzle_f021_16_u8_0111_neon(void);
extern void ff_sws_swizzle_f021_32_u8_0111_neon(void);
extern void ff_sws_swizzle_f0f2_8_u8_0101_neon(void);
extern void ff_sws_swizzle_f0f2_16_u8_0101_neon(void);
extern void ff_sws_swizzle_f0f2_32_u8_0101_neon(void);
extern void ff_sws_swizzle_f0f3_32_u8_0101_neon(void);
extern void ff_sws_swizzle_f0ff_8_u8_0100_neon(void);
extern void ff_sws_swizzle_f0ff_32_u8_0100_neon(void);
extern void ff_sws_swizzle_f102_8_u8_0111_neon(void);
extern void ff_sws_swizzle_f102_16_u8_0111_neon(void);
extern void ff_sws_swizzle_f102_32_u8_0111_neon(void);
extern void ff_sws_swizzle_f123_8_u8_0111_neon(void);
extern void ff_sws_swizzle_f123_16_u8_0111_neon(void);
extern void ff_sws_swizzle_f123_32_u8_0111_neon(void);
extern void ff_sws_swizzle_f12f_8_u8_0110_neon(void);
extern void ff_sws_swizzle_f12f_16_u8_0110_neon(void);
extern void ff_sws_swizzle_f12f_32_u8_0110_neon(void);
extern void ff_sws_swizzle_f132_8_u8_0111_neon(void);
extern void ff_sws_swizzle_f132_16_u8_0111_neon(void);
extern void ff_sws_swizzle_f132_32_u8_0111_neon(void);
extern void ff_sws_swizzle_f321_8_u8_0111_neon(void);
extern void ff_sws_swizzle_f321_16_u8_0111_neon(void);
extern void ff_sws_swizzle_f321_32_u8_0111_neon(void);
extern void ff_sws_swizzle_f3f2_16_u8_0101_neon(void);
extern void ff_sws_swizzle_f3f2_32_u8_0101_neon(void);
extern void ff_sws_swizzle_ff01_8_u8_0011_neon(void);
extern void ff_sws_swizzle_ff01_16_u8_0011_neon(void);
extern void ff_sws_swizzle_ff01_32_u8_0011_neon(void);
extern void ff_sws_swizzle_ff03_8_u8_0011_neon(void);
extern void ff_sws_swizzle_ff03_16_u8_0011_neon(void);
extern void ff_sws_swizzle_ff0f_8_u8_0010_neon(void);
extern void ff_sws_swizzle_ff0f_16_u8_0010_neon(void);
extern void ff_sws_swizzle_ff0f_32_u8_0010_neon(void);
extern void ff_sws_swizzle_ff31_8_u8_0011_neon(void);
extern void ff_sws_swizzle_ff3f_8_u8_0010_neon(void);
extern void ff_sws_swizzle_ff3f_16_u8_0010_neon(void);
extern void ff_sws_swizzle_ff3f_32_u8_0010_neon(void);
extern void ff_sws_swizzle_fff1_32_u8_0001_neon(void);
extern void ff_sws_swizzle_fff2_32_u8_0001_neon(void);
extern void ff_sws_swizzle_fff3_8_u8_0001_neon(void);
extern void ff_sws_swizzle_fff3_16_u8_0001_neon(void);
extern void ff_sws_swizzle_fff3_32_u8_0001_neon(void);
extern void ff_sws_unpack_0121_8_u8_0111_neon(void);
extern void ff_sws_unpack_0121_16_u8_0111_neon(void);
extern void ff_sws_unpack_0233_8_u8_0111_neon(void);
extern void ff_sws_unpack_0233_16_u8_0111_neon(void);
extern void ff_sws_unpack_0332_8_u8_0111_neon(void);
extern void ff_sws_unpack_0332_16_u8_0111_neon(void);
extern void ff_sws_unpack_0444_8_u16_0111_neon(void);
extern void ff_sws_unpack_0444_16_u16_0111_neon(void);
extern void ff_sws_unpack_0555_8_u16_0111_neon(void);
extern void ff_sws_unpack_0555_16_u16_0111_neon(void);
extern void ff_sws_unpack_0565_8_u16_0111_neon(void);
extern void ff_sws_unpack_0565_16_u16_0111_neon(void);
extern void ff_sws_unpack_2aaa_8_u32_0010_neon(void);
extern void ff_sws_unpack_2aaa_8_u32_0111_neon(void);
extern void ff_sws_unpack_aaa2_8_u32_0100_neon(void);
extern void ff_sws_unpack_aaa2_8_u32_1110_neon(void);
extern void ff_sws_pack_0121_8_u8_0111_neon(void);
extern void ff_sws_pack_0121_16_u8_0111_neon(void);
extern void ff_sws_pack_0233_8_u8_0111_neon(void);
extern void ff_sws_pack_0233_16_u8_0111_neon(void);
extern void ff_sws_pack_0332_8_u8_0111_neon(void);
extern void ff_sws_pack_0332_16_u8_0111_neon(void);
extern void ff_sws_pack_0444_8_u16_0111_neon(void);
extern void ff_sws_pack_0444_16_u16_0111_neon(void);
extern void ff_sws_pack_0555_8_u16_0111_neon(void);
extern void ff_sws_pack_0555_16_u16_0111_neon(void);
extern void ff_sws_pack_0565_8_u16_0111_neon(void);
extern void ff_sws_pack_0565_16_u16_0111_neon(void);
extern void ff_sws_pack_2aaa_8_u32_1111_neon(void);
extern void ff_sws_pack_aaa2_8_u32_1111_neon(void);
extern void ff_sws_lshift_1_8_u16_0111_neon(void);
extern void ff_sws_lshift_1_16_u16_0111_neon(void);
extern void ff_sws_lshift_1_16_u16_1110_neon(void);
extern void ff_sws_lshift_2_8_u16_0111_neon(void);
extern void ff_sws_lshift_2_8_u16_1110_neon(void);
extern void ff_sws_lshift_2_8_u32_0111_neon(void);
extern void ff_sws_lshift_2_16_u16_0111_neon(void);
extern void ff_sws_lshift_2_16_u16_1110_neon(void);
extern void ff_sws_lshift_3_16_u16_0111_neon(void);
extern void ff_sws_lshift_4_8_u16_0001_neon(void);
extern void ff_sws_lshift_4_8_u16_0111_neon(void);
extern void ff_sws_lshift_4_8_u16_1110_neon(void);
extern void ff_sws_lshift_4_16_u16_0001_neon(void);
extern void ff_sws_lshift_4_16_u16_0111_neon(void);
extern void ff_sws_lshift_4_16_u16_1110_neon(void);
extern void ff_sws_lshift_5_16_u16_0111_neon(void);
extern void ff_sws_lshift_6_8_u16_0001_neon(void);
extern void ff_sws_lshift_6_8_u16_0111_neon(void);
extern void ff_sws_lshift_6_8_u16_1110_neon(void);
extern void ff_sws_lshift_6_16_u16_0001_neon(void);
extern void ff_sws_lshift_6_16_u16_0111_neon(void);
extern void ff_sws_lshift_6_16_u16_1110_neon(void);
extern void ff_sws_lshift_7_16_u16_0111_neon(void);
extern void ff_sws_lshift_8_16_u16_0111_neon(void);
extern void ff_sws_lshift_8_16_u16_1110_neon(void);
extern void ff_sws_rshift_4_8_u16_0001_neon(void);
extern void ff_sws_rshift_4_8_u16_0010_neon(void);
extern void ff_sws_rshift_4_8_u16_0111_neon(void);
extern void ff_sws_rshift_4_16_u16_0111_neon(void);
extern void ff_sws_rshift_6_8_u16_0001_neon(void);
extern void ff_sws_rshift_6_8_u16_0111_neon(void);
extern void ff_sws_rshift_6_16_u16_0111_neon(void);
extern void ff_sws_clear_8_u8_0001_neon(void);
extern void ff_sws_clear_8_u8_0010_neon(void);
extern void ff_sws_clear_8_u8_0011_neon(void);
extern void ff_sws_clear_8_u8_0101_neon(void);
extern void ff_sws_clear_8_u8_0110_neon(void);
extern void ff_sws_clear_8_u8_1000_neon(void);
extern void ff_sws_clear_8_u8_1011_neon(void);
extern void ff_sws_clear_8_u8_1100_neon(void);
extern void ff_sws_clear_8_u8_1101_neon(void);
extern void ff_sws_clear_8_u8_1110_neon(void);
extern void ff_sws_clear_8_u16_0001_neon(void);
extern void ff_sws_clear_8_u16_0010_neon(void);
extern void ff_sws_clear_8_u16_0110_neon(void);
extern void ff_sws_clear_8_u16_1000_neon(void);
extern void ff_sws_clear_8_u16_1100_neon(void);
extern void ff_sws_clear_8_u16_1101_neon(void);
extern void ff_sws_clear_8_u16_1110_neon(void);
extern void ff_sws_clear_8_u32_0001_neon(void);
extern void ff_sws_clear_8_u32_0010_neon(void);
extern void ff_sws_clear_8_u32_0101_neon(void);
extern void ff_sws_clear_8_u32_1000_neon(void);
extern void ff_sws_clear_8_u32_1010_neon(void);
extern void ff_sws_clear_8_u32_1011_neon(void);
extern void ff_sws_clear_8_u32_1101_neon(void);
extern void ff_sws_clear_16_u8_0001_neon(void);
extern void ff_sws_clear_16_u8_0010_neon(void);
extern void ff_sws_clear_16_u8_0110_neon(void);
extern void ff_sws_clear_16_u8_1000_neon(void);
extern void ff_sws_clear_16_u16_0001_neon(void);
extern void ff_sws_clear_16_u16_0010_neon(void);
extern void ff_sws_clear_16_u16_1000_neon(void);
extern void ff_sws_convert_u8_8_f32_0001_neon(void);
extern void ff_sws_convert_u8_8_f32_0011_neon(void);
extern void ff_sws_convert_u8_8_f32_0111_neon(void);
extern void ff_sws_convert_u8_8_f32_1001_neon(void);
extern void ff_sws_convert_u8_8_f32_1110_neon(void);
extern void ff_sws_convert_u8_8_f32_1111_neon(void);
extern void ff_sws_convert_u8_16_u16_0111_neon(void);
extern void ff_sws_convert_u16_8_u8_0111_neon(void);
extern void ff_sws_convert_u16_8_u32_0111_neon(void);
extern void ff_sws_convert_u16_8_u32_1110_neon(void);
extern void ff_sws_convert_u16_8_f32_0001_neon(void);
extern void ff_sws_convert_u16_8_f32_0011_neon(void);
extern void ff_sws_convert_u16_8_f32_0111_neon(void);
extern void ff_sws_convert_u16_8_f32_1001_neon(void);
extern void ff_sws_convert_u16_8_f32_1110_neon(void);
extern void ff_sws_convert_u16_8_f32_1111_neon(void);
extern void ff_sws_convert_u16_16_u8_0001_neon(void);
extern void ff_sws_convert_u16_16_u8_0111_neon(void);
extern void ff_sws_convert_u16_16_u8_1110_neon(void);
extern void ff_sws_convert_u32_8_u8_0001_neon(void);
extern void ff_sws_convert_u32_8_u8_0111_neon(void);
extern void ff_sws_convert_u32_8_u16_0001_neon(void);
extern void ff_sws_convert_u32_8_u16_0111_neon(void);
extern void ff_sws_convert_u32_8_f32_0001_neon(void);
extern void ff_sws_convert_u32_8_f32_0111_neon(void);
extern void ff_sws_convert_u32_8_f32_1001_neon(void);
extern void ff_sws_convert_u32_8_f32_1110_neon(void);
extern void ff_sws_convert_u32_8_f32_1111_neon(void);
extern void ff_sws_convert_f32_8_u8_0001_neon(void);
extern void ff_sws_convert_f32_8_u8_0010_neon(void);
extern void ff_sws_convert_f32_8_u8_0011_neon(void);
extern void ff_sws_convert_f32_8_u8_0100_neon(void);
extern void ff_sws_convert_f32_8_u8_0111_neon(void);
extern void ff_sws_convert_f32_8_u8_1010_neon(void);
extern void ff_sws_convert_f32_8_u8_1100_neon(void);
extern void ff_sws_convert_f32_8_u8_1110_neon(void);
extern void ff_sws_convert_f32_8_u8_1111_neon(void);
extern void ff_sws_convert_f32_8_u16_0001_neon(void);
extern void ff_sws_convert_f32_8_u16_0010_neon(void);
extern void ff_sws_convert_f32_8_u16_0011_neon(void);
extern void ff_sws_convert_f32_8_u16_0111_neon(void);
extern void ff_sws_convert_f32_8_u16_1110_neon(void);
extern void ff_sws_convert_f32_8_u16_1111_neon(void);
extern void ff_sws_convert_f32_8_u32_0010_neon(void);
extern void ff_sws_convert_f32_8_u32_0100_neon(void);
extern void ff_sws_convert_f32_8_u32_0111_neon(void);
extern void ff_sws_convert_f32_8_u32_1110_neon(void);
extern void ff_sws_expand_u16_16_u8_0001_neon(void);
extern void ff_sws_expand_u16_16_u8_0011_neon(void);
extern void ff_sws_expand_u16_16_u8_0111_neon(void);
extern void ff_sws_expand_u16_16_u8_1110_neon(void);
extern void ff_sws_expand_u16_16_u8_1111_neon(void);
extern void ff_sws_min_8_f32_0001_neon(void);
extern void ff_sws_min_8_f32_0011_neon(void);
extern void ff_sws_min_8_f32_0111_neon(void);
extern void ff_sws_min_8_f32_1001_neon(void);
extern void ff_sws_min_8_f32_1110_neon(void);
extern void ff_sws_min_8_f32_1111_neon(void);
extern void ff_sws_max_8_f32_0001_neon(void);
extern void ff_sws_max_8_f32_0011_neon(void);
extern void ff_sws_max_8_f32_0111_neon(void);
extern void ff_sws_max_8_f32_1001_neon(void);
extern void ff_sws_max_8_f32_1111_neon(void);
extern void ff_sws_scale_8_u32_0001_neon(void);
extern void ff_sws_scale_8_u32_0111_neon(void);
extern void ff_sws_scale_8_f32_0001_neon(void);
extern void ff_sws_scale_8_f32_0011_neon(void);
extern void ff_sws_scale_8_f32_0111_neon(void);
extern void ff_sws_scale_8_f32_1110_neon(void);
extern void ff_sws_scale_8_f32_1111_neon(void);
extern void ff_sws_scale_16_u8_0001_neon(void);
extern void ff_sws_scale_16_u8_0111_neon(void);
extern void ff_sws_scale_16_u16_0001_neon(void);
extern void ff_sws_scale_16_u16_0111_neon(void);
extern void ff_sws_linear_000000000f_0_8_f32_0001_neon(void);
extern void ff_sws_linear_000000000f_1_8_f32_0001_neon(void);
extern void ff_sws_linear_00000000fc_0_8_f32_0001_neon(void);
extern void ff_sws_linear_00000000fc_1_8_f32_0001_neon(void);
extern void ff_sws_linear_00000000ff_0_8_f32_0001_neon(void);
extern void ff_sws_linear_00000000ff_1_8_f32_0001_neon(void);
extern void ff_sws_linear_000000c000_0_8_f32_0010_neon(void);
extern void ff_sws_linear_000000c000_1_8_f32_0010_neon(void);
extern void ff_sws_linear_000373dcc7_0_8_f32_0111_neon(void);
extern void ff_sws_linear_000373dcc7_1_8_f32_0111_neon(void);
extern void ff_sws_linear_0003f3fccf_0_8_f32_0111_neon(void);
extern void ff_sws_linear_0003f3fccf_1_8_f32_0111_neon(void);
extern void ff_sws_linear_000c00c00c_0_8_f32_0111_neon(void);
extern void ff_sws_linear_000c00c00c_1_8_f32_0111_neon(void);
extern void ff_sws_linear_000c30cc0f_0_8_f32_0111_neon(void);
extern void ff_sws_linear_000c30cc0f_1_8_f32_0111_neon(void);
extern void ff_sws_linear_000ff3fcfc_0_8_f32_0111_neon(void);
extern void ff_sws_linear_000ff3fcfc_1_8_f32_0111_neon(void);
extern void ff_sws_linear_000ff3fcff_0_8_f32_0111_neon(void);
extern void ff_sws_linear_000ff3fcff_1_8_f32_0111_neon(void);
extern void ff_sws_linear_c000000000_0_8_f32_1000_neon(void);
extern void ff_sws_linear_c000000000_1_8_f32_1000_neon(void);
extern void ff_sws_linear_c00000000f_0_8_f32_1001_neon(void);
extern void ff_sws_linear_c00000000f_1_8_f32_1001_neon(void);
extern void ff_sws_linear_c0000000fc_0_8_f32_1001_neon(void);
extern void ff_sws_linear_c0000000fc_1_8_f32_1001_neon(void);
extern void ff_sws_linear_c003f3fccf_0_8_f32_1111_neon(void);
extern void ff_sws_linear_c003f3fccf_1_8_f32_1111_neon(void);
extern void ff_sws_linear_c00c00c00c_0_8_f32_1111_neon(void);
extern void ff_sws_linear_c00c00c00c_1_8_f32_1111_neon(void);
extern void ff_sws_linear_c00ff3fcff_0_8_f32_1111_neon(void);
extern void ff_sws_linear_c00ff3fcff_1_8_f32_1111_neon(void);
extern void ff_sws_dither_0325_4_8_f32_1111_neon(void);
extern void ff_sws_dither_032f_4_8_f32_1110_neon(void);
extern void ff_sws_dither_2305_4_8_f32_1111_neon(void);
extern void ff_sws_dither_230f_4_8_f32_1110_neon(void);
extern void ff_sws_dither_3000_4_8_f32_1111_neon(void);
extern void ff_sws_dither_302f_4_8_f32_1110_neon(void);
extern void ff_sws_dither_3ff0_4_8_f32_1001_neon(void);
extern void ff_sws_dither_5023_4_8_f32_1111_neon(void);
extern void ff_sws_dither_5032_4_8_f32_1111_neon(void);
extern void ff_sws_dither_5230_4_8_f32_1111_neon(void);
extern void ff_sws_dither_5ff0_4_8_f32_1001_neon(void);
extern void ff_sws_dither_5fff_4_8_f32_1000_neon(void);
extern void ff_sws_dither_f023_4_8_f32_0111_neon(void);
extern void ff_sws_dither_f032_4_8_f32_0111_neon(void);
extern void ff_sws_dither_f203_4_8_f32_0111_neon(void);
extern void ff_sws_dither_f230_4_8_f32_0111_neon(void);
extern void ff_sws_dither_f2f0_4_8_f32_0101_neon(void);
extern void ff_sws_dither_f2ff_4_8_f32_0100_neon(void);
extern void ff_sws_dither_f302_4_8_f32_0111_neon(void);
extern void ff_sws_dither_ff30_4_8_f32_0011_neon(void);
extern void ff_sws_dither_ff3f_4_8_f32_0010_neon(void);
extern void ff_sws_dither_fff0_4_8_f32_0001_neon(void);

SwsFuncPtr ff_sws_aarch64_lookup(const SwsAArch64OpImplParams *p)
{
    if (p->op == AARCH64_SWS_OP_READ_BIT) {
        if (p->block_size == 8) {
            if (p->type == AARCH64_PIXEL_U8) {
                if (p->mask == 0x0001) return ff_sws_read_bit_8_u8_0001_neon;
                return NULL;
            }
            return NULL;
        }
        if (p->block_size == 16) {
            if (p->type == AARCH64_PIXEL_U8) {
                if (p->mask == 0x0001) return ff_sws_read_bit_16_u8_0001_neon;
                return NULL;
            }
            return NULL;
        }
        return NULL;
    }
    if (p->op == AARCH64_SWS_OP_READ_NIBBLE) {
        if (p->block_size == 8) {
            if (p->type == AARCH64_PIXEL_U8) {
                if (p->mask == 0x0001) return ff_sws_read_nibble_8_u8_0001_neon;
                return NULL;
            }
            return NULL;
        }
        if (p->block_size == 16) {
            if (p->type == AARCH64_PIXEL_U8) {
                if (p->mask == 0x0001) return ff_sws_read_nibble_16_u8_0001_neon;
                return NULL;
            }
            return NULL;
        }
        return NULL;
    }
    if (p->op == AARCH64_SWS_OP_READ_PACKED) {
        if (p->block_size == 8) {
            if (p->type == AARCH64_PIXEL_U8) {
                if (p->mask == 0x0011) return ff_sws_read_packed_8_u8_0011_neon;
                if (p->mask == 0x0111) return ff_sws_read_packed_8_u8_0111_neon;
                if (p->mask == 0x1111) return ff_sws_read_packed_8_u8_1111_neon;
                return NULL;
            }
            if (p->type == AARCH64_PIXEL_U16) {
                if (p->mask == 0x0011) return ff_sws_read_packed_8_u16_0011_neon;
                if (p->mask == 0x0111) return ff_sws_read_packed_8_u16_0111_neon;
                if (p->mask == 0x1111) return ff_sws_read_packed_8_u16_1111_neon;
                return NULL;
            }
            if (p->type == AARCH64_PIXEL_U32) {
                if (p->mask == 0x0011) return ff_sws_read_packed_8_u32_0011_neon;
                if (p->mask == 0x0111) return ff_sws_read_packed_8_u32_0111_neon;
                if (p->mask == 0x1111) return ff_sws_read_packed_8_u32_1111_neon;
                return NULL;
            }
            return NULL;
        }
        if (p->block_size == 16) {
            if (p->type == AARCH64_PIXEL_U8) {
                if (p->mask == 0x0011) return ff_sws_read_packed_16_u8_0011_neon;
                if (p->mask == 0x0111) return ff_sws_read_packed_16_u8_0111_neon;
                if (p->mask == 0x1111) return ff_sws_read_packed_16_u8_1111_neon;
                return NULL;
            }
            if (p->type == AARCH64_PIXEL_U16) {
                if (p->mask == 0x0011) return ff_sws_read_packed_16_u16_0011_neon;
                if (p->mask == 0x0111) return ff_sws_read_packed_16_u16_0111_neon;
                if (p->mask == 0x1111) return ff_sws_read_packed_16_u16_1111_neon;
                return NULL;
            }
            return NULL;
        }
        return NULL;
    }
    if (p->op == AARCH64_SWS_OP_READ_PLANAR) {
        if (p->block_size == 8) {
            if (p->type == AARCH64_PIXEL_U8) {
                if (p->mask == 0x0001) return ff_sws_read_planar_8_u8_0001_neon;
                if (p->mask == 0x0011) return ff_sws_read_planar_8_u8_0011_neon;
                if (p->mask == 0x0111) return ff_sws_read_planar_8_u8_0111_neon;
                if (p->mask == 0x1111) return ff_sws_read_planar_8_u8_1111_neon;
                return NULL;
            }
            if (p->type == AARCH64_PIXEL_U16) {
                if (p->mask == 0x0001) return ff_sws_read_planar_8_u16_0001_neon;
                if (p->mask == 0x0011) return ff_sws_read_planar_8_u16_0011_neon;
                if (p->mask == 0x0111) return ff_sws_read_planar_8_u16_0111_neon;
                if (p->mask == 0x1111) return ff_sws_read_planar_8_u16_1111_neon;
                return NULL;
            }
            if (p->type == AARCH64_PIXEL_U32) {
                if (p->mask == 0x0001) return ff_sws_read_planar_8_u32_0001_neon;
                if (p->mask == 0x0111) return ff_sws_read_planar_8_u32_0111_neon;
                if (p->mask == 0x1111) return ff_sws_read_planar_8_u32_1111_neon;
                return NULL;
            }
            return NULL;
        }
        if (p->block_size == 16) {
            if (p->type == AARCH64_PIXEL_U8) {
                if (p->mask == 0x0001) return ff_sws_read_planar_16_u8_0001_neon;
                if (p->mask == 0x0111) return ff_sws_read_planar_16_u8_0111_neon;
                if (p->mask == 0x1111) return ff_sws_read_planar_16_u8_1111_neon;
                return NULL;
            }
            if (p->type == AARCH64_PIXEL_U16) {
                if (p->mask == 0x0001) return ff_sws_read_planar_16_u16_0001_neon;
                if (p->mask == 0x0111) return ff_sws_read_planar_16_u16_0111_neon;
                if (p->mask == 0x1111) return ff_sws_read_planar_16_u16_1111_neon;
                return NULL;
            }
            return NULL;
        }
        return NULL;
    }
    if (p->op == AARCH64_SWS_OP_WRITE_BIT) {
        if (p->block_size == 8) {
            if (p->type == AARCH64_PIXEL_U8) {
                if (p->mask == 0x0001) return ff_sws_write_bit_8_u8_0001_neon;
                return NULL;
            }
            return NULL;
        }
        if (p->block_size == 16) {
            if (p->type == AARCH64_PIXEL_U8) {
                if (p->mask == 0x0001) return ff_sws_write_bit_16_u8_0001_neon;
                return NULL;
            }
            return NULL;
        }
        return NULL;
    }
    if (p->op == AARCH64_SWS_OP_WRITE_NIBBLE) {
        if (p->block_size == 8) {
            if (p->type == AARCH64_PIXEL_U8) {
                if (p->mask == 0x0001) return ff_sws_write_nibble_8_u8_0001_neon;
                return NULL;
            }
            return NULL;
        }
        if (p->block_size == 16) {
            if (p->type == AARCH64_PIXEL_U8) {
                if (p->mask == 0x0001) return ff_sws_write_nibble_16_u8_0001_neon;
                return NULL;
            }
            return NULL;
        }
        return NULL;
    }
    if (p->op == AARCH64_SWS_OP_WRITE_PACKED) {
        if (p->block_size == 8) {
            if (p->type == AARCH64_PIXEL_U8) {
                if (p->mask == 0x0011) return ff_sws_write_packed_8_u8_0011_neon;
                if (p->mask == 0x0111) return ff_sws_write_packed_8_u8_0111_neon;
                if (p->mask == 0x1111) return ff_sws_write_packed_8_u8_1111_neon;
                return NULL;
            }
            if (p->type == AARCH64_PIXEL_U16) {
                if (p->mask == 0x0011) return ff_sws_write_packed_8_u16_0011_neon;
                if (p->mask == 0x0111) return ff_sws_write_packed_8_u16_0111_neon;
                if (p->mask == 0x1111) return ff_sws_write_packed_8_u16_1111_neon;
                return NULL;
            }
            if (p->type == AARCH64_PIXEL_U32) {
                if (p->mask == 0x0011) return ff_sws_write_packed_8_u32_0011_neon;
                if (p->mask == 0x0111) return ff_sws_write_packed_8_u32_0111_neon;
                if (p->mask == 0x1111) return ff_sws_write_packed_8_u32_1111_neon;
                return NULL;
            }
            return NULL;
        }
        if (p->block_size == 16) {
            if (p->type == AARCH64_PIXEL_U8) {
                if (p->mask == 0x0011) return ff_sws_write_packed_16_u8_0011_neon;
                if (p->mask == 0x0111) return ff_sws_write_packed_16_u8_0111_neon;
                if (p->mask == 0x1111) return ff_sws_write_packed_16_u8_1111_neon;
                return NULL;
            }
            if (p->type == AARCH64_PIXEL_U16) {
                if (p->mask == 0x0011) return ff_sws_write_packed_16_u16_0011_neon;
                if (p->mask == 0x0111) return ff_sws_write_packed_16_u16_0111_neon;
                if (p->mask == 0x1111) return ff_sws_write_packed_16_u16_1111_neon;
                return NULL;
            }
            return NULL;
        }
        return NULL;
    }
    if (p->op == AARCH64_SWS_OP_WRITE_PLANAR) {
        if (p->block_size == 8) {
            if (p->type == AARCH64_PIXEL_U8) {
                if (p->mask == 0x0001) return ff_sws_write_planar_8_u8_0001_neon;
                if (p->mask == 0x0111) return ff_sws_write_planar_8_u8_0111_neon;
                if (p->mask == 0x1111) return ff_sws_write_planar_8_u8_1111_neon;
                return NULL;
            }
            if (p->type == AARCH64_PIXEL_U16) {
                if (p->mask == 0x0001) return ff_sws_write_planar_8_u16_0001_neon;
                if (p->mask == 0x0111) return ff_sws_write_planar_8_u16_0111_neon;
                if (p->mask == 0x1111) return ff_sws_write_planar_8_u16_1111_neon;
                return NULL;
            }
            if (p->type == AARCH64_PIXEL_U32) {
                if (p->mask == 0x0001) return ff_sws_write_planar_8_u32_0001_neon;
                if (p->mask == 0x0111) return ff_sws_write_planar_8_u32_0111_neon;
                if (p->mask == 0x1111) return ff_sws_write_planar_8_u32_1111_neon;
                return NULL;
            }
            return NULL;
        }
        if (p->block_size == 16) {
            if (p->type == AARCH64_PIXEL_U8) {
                if (p->mask == 0x0001) return ff_sws_write_planar_16_u8_0001_neon;
                if (p->mask == 0x0111) return ff_sws_write_planar_16_u8_0111_neon;
                if (p->mask == 0x1111) return ff_sws_write_planar_16_u8_1111_neon;
                return NULL;
            }
            if (p->type == AARCH64_PIXEL_U16) {
                if (p->mask == 0x0001) return ff_sws_write_planar_16_u16_0001_neon;
                if (p->mask == 0x0111) return ff_sws_write_planar_16_u16_0111_neon;
                if (p->mask == 0x1111) return ff_sws_write_planar_16_u16_1111_neon;
                return NULL;
            }
            return NULL;
        }
        return NULL;
    }
    if (p->op == AARCH64_SWS_OP_SWAP_BYTES) {
        if (p->block_size == 8) {
            if (p->type == AARCH64_PIXEL_U16) {
                if (p->mask == 0x0001) return ff_sws_swap_bytes_8_u16_0001_neon;
                if (p->mask == 0x0010) return ff_sws_swap_bytes_8_u16_0010_neon;
                if (p->mask == 0x0011) return ff_sws_swap_bytes_8_u16_0011_neon;
                if (p->mask == 0x0111) return ff_sws_swap_bytes_8_u16_0111_neon;
                if (p->mask == 0x1001) return ff_sws_swap_bytes_8_u16_1001_neon;
                if (p->mask == 0x1110) return ff_sws_swap_bytes_8_u16_1110_neon;
                if (p->mask == 0x1111) return ff_sws_swap_bytes_8_u16_1111_neon;
                return NULL;
            }
            if (p->type == AARCH64_PIXEL_U32) {
                if (p->mask == 0x0001) return ff_sws_swap_bytes_8_u32_0001_neon;
                if (p->mask == 0x0011) return ff_sws_swap_bytes_8_u32_0011_neon;
                if (p->mask == 0x0111) return ff_sws_swap_bytes_8_u32_0111_neon;
                if (p->mask == 0x1001) return ff_sws_swap_bytes_8_u32_1001_neon;
                if (p->mask == 0x1110) return ff_sws_swap_bytes_8_u32_1110_neon;
                if (p->mask == 0x1111) return ff_sws_swap_bytes_8_u32_1111_neon;
                return NULL;
            }
            return NULL;
        }
        if (p->block_size == 16) {
            if (p->type == AARCH64_PIXEL_U16) {
                if (p->mask == 0x0001) return ff_sws_swap_bytes_16_u16_0001_neon;
                if (p->mask == 0x0011) return ff_sws_swap_bytes_16_u16_0011_neon;
                if (p->mask == 0x0111) return ff_sws_swap_bytes_16_u16_0111_neon;
                if (p->mask == 0x1110) return ff_sws_swap_bytes_16_u16_1110_neon;
                if (p->mask == 0x1111) return ff_sws_swap_bytes_16_u16_1111_neon;
                return NULL;
            }
            return NULL;
        }
        return NULL;
    }
    if (p->op == AARCH64_SWS_OP_SWIZZLE) {
        if (p->swizzle == 0x0001) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1111) return ff_sws_swizzle_0001_8_u8_1111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1111) return ff_sws_swizzle_0001_16_u8_1111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0x000f) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1110) return ff_sws_swizzle_000f_8_u8_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1110) return ff_sws_swizzle_000f_16_u8_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1110) return ff_sws_swizzle_000f_32_u8_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0x0123) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1111) return ff_sws_swizzle_0123_8_u8_1111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1111) return ff_sws_swizzle_0123_16_u8_1111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1111) return ff_sws_swizzle_0123_32_u8_1111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0x012f) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1110) return ff_sws_swizzle_012f_8_u8_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1110) return ff_sws_swizzle_012f_16_u8_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1110) return ff_sws_swizzle_012f_32_u8_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0x0321) {
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1111) return ff_sws_swizzle_0321_16_u8_1111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1111) return ff_sws_swizzle_0321_32_u8_1111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0x03f2) {
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1101) return ff_sws_swizzle_03f2_16_u8_1101_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0x0ff1) {
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1001) return ff_sws_swizzle_0ff1_32_u8_1001_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0x0fff) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1000) return ff_sws_swizzle_0fff_8_u8_1000_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1000) return ff_sws_swizzle_0fff_16_u8_1000_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1000) return ff_sws_swizzle_0fff_32_u8_1000_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0x100f) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1110) return ff_sws_swizzle_100f_8_u8_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1110) return ff_sws_swizzle_100f_16_u8_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1110) return ff_sws_swizzle_100f_32_u8_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0x1023) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1111) return ff_sws_swizzle_1023_8_u8_1111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1111) return ff_sws_swizzle_1023_32_u8_1111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0x102f) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1110) return ff_sws_swizzle_102f_8_u8_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1110) return ff_sws_swizzle_102f_32_u8_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0x132f) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1110) return ff_sws_swizzle_132f_8_u8_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1110) return ff_sws_swizzle_132f_16_u8_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0x1f0f) {
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1010) return ff_sws_swizzle_1f0f_32_u8_1010_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0x1f3f) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1010) return ff_sws_swizzle_1f3f_8_u8_1010_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1010) return ff_sws_swizzle_1f3f_16_u8_1010_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1010) return ff_sws_swizzle_1f3f_32_u8_1010_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0x1fff) {
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1000) return ff_sws_swizzle_1fff_32_u8_1000_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0x20f3) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1101) return ff_sws_swizzle_20f3_8_u8_1101_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1101) return ff_sws_swizzle_20f3_16_u8_1101_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0x20ff) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1100) return ff_sws_swizzle_20ff_8_u8_1100_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1100) return ff_sws_swizzle_20ff_32_u8_1100_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0x2103) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1111) return ff_sws_swizzle_2103_8_u8_1111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1111) return ff_sws_swizzle_2103_16_u8_1111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1111) return ff_sws_swizzle_2103_32_u8_1111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0x210f) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1110) return ff_sws_swizzle_210f_8_u8_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1110) return ff_sws_swizzle_210f_16_u8_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x1110) return ff_sws_swizzle_210f_32_u8_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0xf00f) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0110) return ff_sws_swizzle_f00f_8_u8_0110_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0110) return ff_sws_swizzle_f00f_16_u8_0110_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0110) return ff_sws_swizzle_f00f_32_u8_0110_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0xf021) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_swizzle_f021_8_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_swizzle_f021_16_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_swizzle_f021_32_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0xf0f2) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0101) return ff_sws_swizzle_f0f2_8_u8_0101_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0101) return ff_sws_swizzle_f0f2_16_u8_0101_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0101) return ff_sws_swizzle_f0f2_32_u8_0101_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0xf0f3) {
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0101) return ff_sws_swizzle_f0f3_32_u8_0101_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0xf0ff) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0100) return ff_sws_swizzle_f0ff_8_u8_0100_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0100) return ff_sws_swizzle_f0ff_32_u8_0100_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0xf102) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_swizzle_f102_8_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_swizzle_f102_16_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_swizzle_f102_32_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0xf123) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_swizzle_f123_8_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_swizzle_f123_16_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_swizzle_f123_32_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0xf12f) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0110) return ff_sws_swizzle_f12f_8_u8_0110_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0110) return ff_sws_swizzle_f12f_16_u8_0110_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0110) return ff_sws_swizzle_f12f_32_u8_0110_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0xf132) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_swizzle_f132_8_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_swizzle_f132_16_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_swizzle_f132_32_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0xf321) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_swizzle_f321_8_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_swizzle_f321_16_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_swizzle_f321_32_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0xf3f2) {
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0101) return ff_sws_swizzle_f3f2_16_u8_0101_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0101) return ff_sws_swizzle_f3f2_32_u8_0101_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0xff01) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0011) return ff_sws_swizzle_ff01_8_u8_0011_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0011) return ff_sws_swizzle_ff01_16_u8_0011_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0011) return ff_sws_swizzle_ff01_32_u8_0011_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0xff03) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0011) return ff_sws_swizzle_ff03_8_u8_0011_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0011) return ff_sws_swizzle_ff03_16_u8_0011_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0xff0f) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0010) return ff_sws_swizzle_ff0f_8_u8_0010_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0010) return ff_sws_swizzle_ff0f_16_u8_0010_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0010) return ff_sws_swizzle_ff0f_32_u8_0010_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0xff31) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0011) return ff_sws_swizzle_ff31_8_u8_0011_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0xff3f) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0010) return ff_sws_swizzle_ff3f_8_u8_0010_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0010) return ff_sws_swizzle_ff3f_16_u8_0010_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0010) return ff_sws_swizzle_ff3f_32_u8_0010_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0xfff1) {
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0001) return ff_sws_swizzle_fff1_32_u8_0001_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0xfff2) {
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0001) return ff_sws_swizzle_fff2_32_u8_0001_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->swizzle == 0xfff3) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0001) return ff_sws_swizzle_fff3_8_u8_0001_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0001) return ff_sws_swizzle_fff3_16_u8_0001_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 32) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0001) return ff_sws_swizzle_fff3_32_u8_0001_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        return NULL;
    }
    if (p->op == AARCH64_SWS_OP_UNPACK) {
        if (p->pack == 0x0121) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_unpack_0121_8_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_unpack_0121_16_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->pack == 0x0233) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_unpack_0233_8_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_unpack_0233_16_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->pack == 0x0332) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_unpack_0332_8_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_unpack_0332_16_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->pack == 0x0444) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0111) return ff_sws_unpack_0444_8_u16_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0111) return ff_sws_unpack_0444_16_u16_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->pack == 0x0555) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0111) return ff_sws_unpack_0555_8_u16_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0111) return ff_sws_unpack_0555_16_u16_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->pack == 0x0565) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0111) return ff_sws_unpack_0565_8_u16_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0111) return ff_sws_unpack_0565_16_u16_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->pack == 0x2aaa) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U32) {
                    if (p->mask == 0x0010) return ff_sws_unpack_2aaa_8_u32_0010_neon;
                    if (p->mask == 0x0111) return ff_sws_unpack_2aaa_8_u32_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->pack == 0xaaa2) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U32) {
                    if (p->mask == 0x0100) return ff_sws_unpack_aaa2_8_u32_0100_neon;
                    if (p->mask == 0x1110) return ff_sws_unpack_aaa2_8_u32_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        return NULL;
    }
    if (p->op == AARCH64_SWS_OP_PACK) {
        if (p->pack == 0x0121) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_pack_0121_8_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_pack_0121_16_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->pack == 0x0233) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_pack_0233_8_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_pack_0233_16_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->pack == 0x0332) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_pack_0332_8_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_pack_0332_16_u8_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->pack == 0x0444) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0111) return ff_sws_pack_0444_8_u16_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0111) return ff_sws_pack_0444_16_u16_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->pack == 0x0555) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0111) return ff_sws_pack_0555_8_u16_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0111) return ff_sws_pack_0555_16_u16_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->pack == 0x0565) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0111) return ff_sws_pack_0565_8_u16_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0111) return ff_sws_pack_0565_16_u16_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->pack == 0x2aaa) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U32) {
                    if (p->mask == 0x1111) return ff_sws_pack_2aaa_8_u32_1111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->pack == 0xaaa2) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U32) {
                    if (p->mask == 0x1111) return ff_sws_pack_aaa2_8_u32_1111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        return NULL;
    }
    if (p->op == AARCH64_SWS_OP_LSHIFT) {
        if (p->shift == 1) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0111) return ff_sws_lshift_1_8_u16_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0111) return ff_sws_lshift_1_16_u16_0111_neon;
                    if (p->mask == 0x1110) return ff_sws_lshift_1_16_u16_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->shift == 2) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0111) return ff_sws_lshift_2_8_u16_0111_neon;
                    if (p->mask == 0x1110) return ff_sws_lshift_2_8_u16_1110_neon;
                    return NULL;
                }
                if (p->type == AARCH64_PIXEL_U32) {
                    if (p->mask == 0x0111) return ff_sws_lshift_2_8_u32_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0111) return ff_sws_lshift_2_16_u16_0111_neon;
                    if (p->mask == 0x1110) return ff_sws_lshift_2_16_u16_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->shift == 3) {
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0111) return ff_sws_lshift_3_16_u16_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->shift == 4) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0001) return ff_sws_lshift_4_8_u16_0001_neon;
                    if (p->mask == 0x0111) return ff_sws_lshift_4_8_u16_0111_neon;
                    if (p->mask == 0x1110) return ff_sws_lshift_4_8_u16_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0001) return ff_sws_lshift_4_16_u16_0001_neon;
                    if (p->mask == 0x0111) return ff_sws_lshift_4_16_u16_0111_neon;
                    if (p->mask == 0x1110) return ff_sws_lshift_4_16_u16_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->shift == 5) {
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0111) return ff_sws_lshift_5_16_u16_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->shift == 6) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0001) return ff_sws_lshift_6_8_u16_0001_neon;
                    if (p->mask == 0x0111) return ff_sws_lshift_6_8_u16_0111_neon;
                    if (p->mask == 0x1110) return ff_sws_lshift_6_8_u16_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0001) return ff_sws_lshift_6_16_u16_0001_neon;
                    if (p->mask == 0x0111) return ff_sws_lshift_6_16_u16_0111_neon;
                    if (p->mask == 0x1110) return ff_sws_lshift_6_16_u16_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->shift == 7) {
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0111) return ff_sws_lshift_7_16_u16_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->shift == 8) {
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0111) return ff_sws_lshift_8_16_u16_0111_neon;
                    if (p->mask == 0x1110) return ff_sws_lshift_8_16_u16_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        return NULL;
    }
    if (p->op == AARCH64_SWS_OP_RSHIFT) {
        if (p->shift == 4) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0001) return ff_sws_rshift_4_8_u16_0001_neon;
                    if (p->mask == 0x0010) return ff_sws_rshift_4_8_u16_0010_neon;
                    if (p->mask == 0x0111) return ff_sws_rshift_4_8_u16_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0111) return ff_sws_rshift_4_16_u16_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->shift == 6) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0001) return ff_sws_rshift_6_8_u16_0001_neon;
                    if (p->mask == 0x0111) return ff_sws_rshift_6_8_u16_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0111) return ff_sws_rshift_6_16_u16_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        return NULL;
    }
    if (p->op == AARCH64_SWS_OP_CLEAR) {
        if (p->block_size == 8) {
            if (p->type == AARCH64_PIXEL_U8) {
                if (p->mask == 0x0001) return ff_sws_clear_8_u8_0001_neon;
                if (p->mask == 0x0010) return ff_sws_clear_8_u8_0010_neon;
                if (p->mask == 0x0011) return ff_sws_clear_8_u8_0011_neon;
                if (p->mask == 0x0101) return ff_sws_clear_8_u8_0101_neon;
                if (p->mask == 0x0110) return ff_sws_clear_8_u8_0110_neon;
                if (p->mask == 0x1000) return ff_sws_clear_8_u8_1000_neon;
                if (p->mask == 0x1011) return ff_sws_clear_8_u8_1011_neon;
                if (p->mask == 0x1100) return ff_sws_clear_8_u8_1100_neon;
                if (p->mask == 0x1101) return ff_sws_clear_8_u8_1101_neon;
                if (p->mask == 0x1110) return ff_sws_clear_8_u8_1110_neon;
                return NULL;
            }
            if (p->type == AARCH64_PIXEL_U16) {
                if (p->mask == 0x0001) return ff_sws_clear_8_u16_0001_neon;
                if (p->mask == 0x0010) return ff_sws_clear_8_u16_0010_neon;
                if (p->mask == 0x0110) return ff_sws_clear_8_u16_0110_neon;
                if (p->mask == 0x1000) return ff_sws_clear_8_u16_1000_neon;
                if (p->mask == 0x1100) return ff_sws_clear_8_u16_1100_neon;
                if (p->mask == 0x1101) return ff_sws_clear_8_u16_1101_neon;
                if (p->mask == 0x1110) return ff_sws_clear_8_u16_1110_neon;
                return NULL;
            }
            if (p->type == AARCH64_PIXEL_U32) {
                if (p->mask == 0x0001) return ff_sws_clear_8_u32_0001_neon;
                if (p->mask == 0x0010) return ff_sws_clear_8_u32_0010_neon;
                if (p->mask == 0x0101) return ff_sws_clear_8_u32_0101_neon;
                if (p->mask == 0x1000) return ff_sws_clear_8_u32_1000_neon;
                if (p->mask == 0x1010) return ff_sws_clear_8_u32_1010_neon;
                if (p->mask == 0x1011) return ff_sws_clear_8_u32_1011_neon;
                if (p->mask == 0x1101) return ff_sws_clear_8_u32_1101_neon;
                return NULL;
            }
            return NULL;
        }
        if (p->block_size == 16) {
            if (p->type == AARCH64_PIXEL_U8) {
                if (p->mask == 0x0001) return ff_sws_clear_16_u8_0001_neon;
                if (p->mask == 0x0010) return ff_sws_clear_16_u8_0010_neon;
                if (p->mask == 0x0110) return ff_sws_clear_16_u8_0110_neon;
                if (p->mask == 0x1000) return ff_sws_clear_16_u8_1000_neon;
                return NULL;
            }
            if (p->type == AARCH64_PIXEL_U16) {
                if (p->mask == 0x0001) return ff_sws_clear_16_u16_0001_neon;
                if (p->mask == 0x0010) return ff_sws_clear_16_u16_0010_neon;
                if (p->mask == 0x1000) return ff_sws_clear_16_u16_1000_neon;
                return NULL;
            }
            return NULL;
        }
        return NULL;
    }
    if (p->op == AARCH64_SWS_OP_CONVERT) {
        if (p->to_type == AARCH64_PIXEL_U8) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_F32) {
                    if (p->mask == 0x0001) return ff_sws_convert_u8_8_f32_0001_neon;
                    if (p->mask == 0x0011) return ff_sws_convert_u8_8_f32_0011_neon;
                    if (p->mask == 0x0111) return ff_sws_convert_u8_8_f32_0111_neon;
                    if (p->mask == 0x1001) return ff_sws_convert_u8_8_f32_1001_neon;
                    if (p->mask == 0x1110) return ff_sws_convert_u8_8_f32_1110_neon;
                    if (p->mask == 0x1111) return ff_sws_convert_u8_8_f32_1111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0111) return ff_sws_convert_u8_16_u16_0111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->to_type == AARCH64_PIXEL_U16) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0111) return ff_sws_convert_u16_8_u8_0111_neon;
                    return NULL;
                }
                if (p->type == AARCH64_PIXEL_U32) {
                    if (p->mask == 0x0111) return ff_sws_convert_u16_8_u32_0111_neon;
                    if (p->mask == 0x1110) return ff_sws_convert_u16_8_u32_1110_neon;
                    return NULL;
                }
                if (p->type == AARCH64_PIXEL_F32) {
                    if (p->mask == 0x0001) return ff_sws_convert_u16_8_f32_0001_neon;
                    if (p->mask == 0x0011) return ff_sws_convert_u16_8_f32_0011_neon;
                    if (p->mask == 0x0111) return ff_sws_convert_u16_8_f32_0111_neon;
                    if (p->mask == 0x1001) return ff_sws_convert_u16_8_f32_1001_neon;
                    if (p->mask == 0x1110) return ff_sws_convert_u16_8_f32_1110_neon;
                    if (p->mask == 0x1111) return ff_sws_convert_u16_8_f32_1111_neon;
                    return NULL;
                }
                return NULL;
            }
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0001) return ff_sws_convert_u16_16_u8_0001_neon;
                    if (p->mask == 0x0111) return ff_sws_convert_u16_16_u8_0111_neon;
                    if (p->mask == 0x1110) return ff_sws_convert_u16_16_u8_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->to_type == AARCH64_PIXEL_U32) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0001) return ff_sws_convert_u32_8_u8_0001_neon;
                    if (p->mask == 0x0111) return ff_sws_convert_u32_8_u8_0111_neon;
                    return NULL;
                }
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0001) return ff_sws_convert_u32_8_u16_0001_neon;
                    if (p->mask == 0x0111) return ff_sws_convert_u32_8_u16_0111_neon;
                    return NULL;
                }
                if (p->type == AARCH64_PIXEL_F32) {
                    if (p->mask == 0x0001) return ff_sws_convert_u32_8_f32_0001_neon;
                    if (p->mask == 0x0111) return ff_sws_convert_u32_8_f32_0111_neon;
                    if (p->mask == 0x1001) return ff_sws_convert_u32_8_f32_1001_neon;
                    if (p->mask == 0x1110) return ff_sws_convert_u32_8_f32_1110_neon;
                    if (p->mask == 0x1111) return ff_sws_convert_u32_8_f32_1111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->to_type == AARCH64_PIXEL_F32) {
            if (p->block_size == 8) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0001) return ff_sws_convert_f32_8_u8_0001_neon;
                    if (p->mask == 0x0010) return ff_sws_convert_f32_8_u8_0010_neon;
                    if (p->mask == 0x0011) return ff_sws_convert_f32_8_u8_0011_neon;
                    if (p->mask == 0x0100) return ff_sws_convert_f32_8_u8_0100_neon;
                    if (p->mask == 0x0111) return ff_sws_convert_f32_8_u8_0111_neon;
                    if (p->mask == 0x1010) return ff_sws_convert_f32_8_u8_1010_neon;
                    if (p->mask == 0x1100) return ff_sws_convert_f32_8_u8_1100_neon;
                    if (p->mask == 0x1110) return ff_sws_convert_f32_8_u8_1110_neon;
                    if (p->mask == 0x1111) return ff_sws_convert_f32_8_u8_1111_neon;
                    return NULL;
                }
                if (p->type == AARCH64_PIXEL_U16) {
                    if (p->mask == 0x0001) return ff_sws_convert_f32_8_u16_0001_neon;
                    if (p->mask == 0x0010) return ff_sws_convert_f32_8_u16_0010_neon;
                    if (p->mask == 0x0011) return ff_sws_convert_f32_8_u16_0011_neon;
                    if (p->mask == 0x0111) return ff_sws_convert_f32_8_u16_0111_neon;
                    if (p->mask == 0x1110) return ff_sws_convert_f32_8_u16_1110_neon;
                    if (p->mask == 0x1111) return ff_sws_convert_f32_8_u16_1111_neon;
                    return NULL;
                }
                if (p->type == AARCH64_PIXEL_U32) {
                    if (p->mask == 0x0010) return ff_sws_convert_f32_8_u32_0010_neon;
                    if (p->mask == 0x0100) return ff_sws_convert_f32_8_u32_0100_neon;
                    if (p->mask == 0x0111) return ff_sws_convert_f32_8_u32_0111_neon;
                    if (p->mask == 0x1110) return ff_sws_convert_f32_8_u32_1110_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        return NULL;
    }
    if (p->op == AARCH64_SWS_OP_EXPAND) {
        if (p->to_type == AARCH64_PIXEL_U16) {
            if (p->block_size == 16) {
                if (p->type == AARCH64_PIXEL_U8) {
                    if (p->mask == 0x0001) return ff_sws_expand_u16_16_u8_0001_neon;
                    if (p->mask == 0x0011) return ff_sws_expand_u16_16_u8_0011_neon;
                    if (p->mask == 0x0111) return ff_sws_expand_u16_16_u8_0111_neon;
                    if (p->mask == 0x1110) return ff_sws_expand_u16_16_u8_1110_neon;
                    if (p->mask == 0x1111) return ff_sws_expand_u16_16_u8_1111_neon;
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        return NULL;
    }
    if (p->op == AARCH64_SWS_OP_MIN) {
        if (p->block_size == 8) {
            if (p->type == AARCH64_PIXEL_F32) {
                if (p->mask == 0x0001) return ff_sws_min_8_f32_0001_neon;
                if (p->mask == 0x0011) return ff_sws_min_8_f32_0011_neon;
                if (p->mask == 0x0111) return ff_sws_min_8_f32_0111_neon;
                if (p->mask == 0x1001) return ff_sws_min_8_f32_1001_neon;
                if (p->mask == 0x1110) return ff_sws_min_8_f32_1110_neon;
                if (p->mask == 0x1111) return ff_sws_min_8_f32_1111_neon;
                return NULL;
            }
            return NULL;
        }
        return NULL;
    }
    if (p->op == AARCH64_SWS_OP_MAX) {
        if (p->block_size == 8) {
            if (p->type == AARCH64_PIXEL_F32) {
                if (p->mask == 0x0001) return ff_sws_max_8_f32_0001_neon;
                if (p->mask == 0x0011) return ff_sws_max_8_f32_0011_neon;
                if (p->mask == 0x0111) return ff_sws_max_8_f32_0111_neon;
                if (p->mask == 0x1001) return ff_sws_max_8_f32_1001_neon;
                if (p->mask == 0x1111) return ff_sws_max_8_f32_1111_neon;
                return NULL;
            }
            return NULL;
        }
        return NULL;
    }
    if (p->op == AARCH64_SWS_OP_SCALE) {
        if (p->block_size == 8) {
            if (p->type == AARCH64_PIXEL_U32) {
                if (p->mask == 0x0001) return ff_sws_scale_8_u32_0001_neon;
                if (p->mask == 0x0111) return ff_sws_scale_8_u32_0111_neon;
                return NULL;
            }
            if (p->type == AARCH64_PIXEL_F32) {
                if (p->mask == 0x0001) return ff_sws_scale_8_f32_0001_neon;
                if (p->mask == 0x0011) return ff_sws_scale_8_f32_0011_neon;
                if (p->mask == 0x0111) return ff_sws_scale_8_f32_0111_neon;
                if (p->mask == 0x1110) return ff_sws_scale_8_f32_1110_neon;
                if (p->mask == 0x1111) return ff_sws_scale_8_f32_1111_neon;
                return NULL;
            }
            return NULL;
        }
        if (p->block_size == 16) {
            if (p->type == AARCH64_PIXEL_U8) {
                if (p->mask == 0x0001) return ff_sws_scale_16_u8_0001_neon;
                if (p->mask == 0x0111) return ff_sws_scale_16_u8_0111_neon;
                return NULL;
            }
            if (p->type == AARCH64_PIXEL_U16) {
                if (p->mask == 0x0001) return ff_sws_scale_16_u16_0001_neon;
                if (p->mask == 0x0111) return ff_sws_scale_16_u16_0111_neon;
                return NULL;
            }
            return NULL;
        }
        return NULL;
    }
    if (p->op == AARCH64_SWS_OP_LINEAR) {
        if (p->linear.mask == 0x000000000fULL) {
            if (p->linear.fmla == 0) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0001) return ff_sws_linear_000000000f_0_8_f32_0001_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            if (p->linear.fmla == 1) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0001) return ff_sws_linear_000000000f_1_8_f32_0001_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->linear.mask == 0x00000000fcULL) {
            if (p->linear.fmla == 0) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0001) return ff_sws_linear_00000000fc_0_8_f32_0001_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            if (p->linear.fmla == 1) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0001) return ff_sws_linear_00000000fc_1_8_f32_0001_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->linear.mask == 0x00000000ffULL) {
            if (p->linear.fmla == 0) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0001) return ff_sws_linear_00000000ff_0_8_f32_0001_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            if (p->linear.fmla == 1) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0001) return ff_sws_linear_00000000ff_1_8_f32_0001_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->linear.mask == 0x000000c000ULL) {
            if (p->linear.fmla == 0) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0010) return ff_sws_linear_000000c000_0_8_f32_0010_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            if (p->linear.fmla == 1) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0010) return ff_sws_linear_000000c000_1_8_f32_0010_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->linear.mask == 0x000373dcc7ULL) {
            if (p->linear.fmla == 0) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0111) return ff_sws_linear_000373dcc7_0_8_f32_0111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            if (p->linear.fmla == 1) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0111) return ff_sws_linear_000373dcc7_1_8_f32_0111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->linear.mask == 0x0003f3fccfULL) {
            if (p->linear.fmla == 0) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0111) return ff_sws_linear_0003f3fccf_0_8_f32_0111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            if (p->linear.fmla == 1) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0111) return ff_sws_linear_0003f3fccf_1_8_f32_0111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->linear.mask == 0x000c00c00cULL) {
            if (p->linear.fmla == 0) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0111) return ff_sws_linear_000c00c00c_0_8_f32_0111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            if (p->linear.fmla == 1) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0111) return ff_sws_linear_000c00c00c_1_8_f32_0111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->linear.mask == 0x000c30cc0fULL) {
            if (p->linear.fmla == 0) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0111) return ff_sws_linear_000c30cc0f_0_8_f32_0111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            if (p->linear.fmla == 1) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0111) return ff_sws_linear_000c30cc0f_1_8_f32_0111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->linear.mask == 0x000ff3fcfcULL) {
            if (p->linear.fmla == 0) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0111) return ff_sws_linear_000ff3fcfc_0_8_f32_0111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            if (p->linear.fmla == 1) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0111) return ff_sws_linear_000ff3fcfc_1_8_f32_0111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->linear.mask == 0x000ff3fcffULL) {
            if (p->linear.fmla == 0) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0111) return ff_sws_linear_000ff3fcff_0_8_f32_0111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            if (p->linear.fmla == 1) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0111) return ff_sws_linear_000ff3fcff_1_8_f32_0111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->linear.mask == 0xc000000000ULL) {
            if (p->linear.fmla == 0) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x1000) return ff_sws_linear_c000000000_0_8_f32_1000_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            if (p->linear.fmla == 1) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x1000) return ff_sws_linear_c000000000_1_8_f32_1000_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->linear.mask == 0xc00000000fULL) {
            if (p->linear.fmla == 0) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x1001) return ff_sws_linear_c00000000f_0_8_f32_1001_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            if (p->linear.fmla == 1) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x1001) return ff_sws_linear_c00000000f_1_8_f32_1001_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->linear.mask == 0xc0000000fcULL) {
            if (p->linear.fmla == 0) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x1001) return ff_sws_linear_c0000000fc_0_8_f32_1001_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            if (p->linear.fmla == 1) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x1001) return ff_sws_linear_c0000000fc_1_8_f32_1001_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->linear.mask == 0xc003f3fccfULL) {
            if (p->linear.fmla == 0) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x1111) return ff_sws_linear_c003f3fccf_0_8_f32_1111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            if (p->linear.fmla == 1) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x1111) return ff_sws_linear_c003f3fccf_1_8_f32_1111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->linear.mask == 0xc00c00c00cULL) {
            if (p->linear.fmla == 0) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x1111) return ff_sws_linear_c00c00c00c_0_8_f32_1111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            if (p->linear.fmla == 1) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x1111) return ff_sws_linear_c00c00c00c_1_8_f32_1111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->linear.mask == 0xc00ff3fcffULL) {
            if (p->linear.fmla == 0) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x1111) return ff_sws_linear_c00ff3fcff_0_8_f32_1111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            if (p->linear.fmla == 1) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x1111) return ff_sws_linear_c00ff3fcff_1_8_f32_1111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        return NULL;
    }
    if (p->op == AARCH64_SWS_OP_DITHER) {
        if (p->dither.y_offset == 0x0325) {
            if (p->dither.size_log2 == 4) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x1111) return ff_sws_dither_0325_4_8_f32_1111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->dither.y_offset == 0x032f) {
            if (p->dither.size_log2 == 4) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x1110) return ff_sws_dither_032f_4_8_f32_1110_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->dither.y_offset == 0x2305) {
            if (p->dither.size_log2 == 4) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x1111) return ff_sws_dither_2305_4_8_f32_1111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->dither.y_offset == 0x230f) {
            if (p->dither.size_log2 == 4) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x1110) return ff_sws_dither_230f_4_8_f32_1110_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->dither.y_offset == 0x3000) {
            if (p->dither.size_log2 == 4) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x1111) return ff_sws_dither_3000_4_8_f32_1111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->dither.y_offset == 0x302f) {
            if (p->dither.size_log2 == 4) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x1110) return ff_sws_dither_302f_4_8_f32_1110_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->dither.y_offset == 0x3ff0) {
            if (p->dither.size_log2 == 4) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x1001) return ff_sws_dither_3ff0_4_8_f32_1001_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->dither.y_offset == 0x5023) {
            if (p->dither.size_log2 == 4) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x1111) return ff_sws_dither_5023_4_8_f32_1111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->dither.y_offset == 0x5032) {
            if (p->dither.size_log2 == 4) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x1111) return ff_sws_dither_5032_4_8_f32_1111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->dither.y_offset == 0x5230) {
            if (p->dither.size_log2 == 4) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x1111) return ff_sws_dither_5230_4_8_f32_1111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->dither.y_offset == 0x5ff0) {
            if (p->dither.size_log2 == 4) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x1001) return ff_sws_dither_5ff0_4_8_f32_1001_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->dither.y_offset == 0x5fff) {
            if (p->dither.size_log2 == 4) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x1000) return ff_sws_dither_5fff_4_8_f32_1000_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->dither.y_offset == 0xf023) {
            if (p->dither.size_log2 == 4) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0111) return ff_sws_dither_f023_4_8_f32_0111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->dither.y_offset == 0xf032) {
            if (p->dither.size_log2 == 4) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0111) return ff_sws_dither_f032_4_8_f32_0111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->dither.y_offset == 0xf203) {
            if (p->dither.size_log2 == 4) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0111) return ff_sws_dither_f203_4_8_f32_0111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->dither.y_offset == 0xf230) {
            if (p->dither.size_log2 == 4) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0111) return ff_sws_dither_f230_4_8_f32_0111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->dither.y_offset == 0xf2f0) {
            if (p->dither.size_log2 == 4) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0101) return ff_sws_dither_f2f0_4_8_f32_0101_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->dither.y_offset == 0xf2ff) {
            if (p->dither.size_log2 == 4) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0100) return ff_sws_dither_f2ff_4_8_f32_0100_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->dither.y_offset == 0xf302) {
            if (p->dither.size_log2 == 4) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0111) return ff_sws_dither_f302_4_8_f32_0111_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->dither.y_offset == 0xff30) {
            if (p->dither.size_log2 == 4) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0011) return ff_sws_dither_ff30_4_8_f32_0011_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->dither.y_offset == 0xff3f) {
            if (p->dither.size_log2 == 4) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0010) return ff_sws_dither_ff3f_4_8_f32_0010_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        if (p->dither.y_offset == 0xfff0) {
            if (p->dither.size_log2 == 4) {
                if (p->block_size == 8) {
                    if (p->type == AARCH64_PIXEL_F32) {
                        if (p->mask == 0x0001) return ff_sws_dither_fff0_4_8_f32_0001_neon;
                        return NULL;
                    }
                    return NULL;
                }
                return NULL;
            }
            return NULL;
        }
        return NULL;
    }
    return NULL;
}
