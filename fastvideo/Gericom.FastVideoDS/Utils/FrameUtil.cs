using System;
using System.Runtime.CompilerServices;

namespace Gericom.FastVideoDS.Utils
{
    // Scalar (portable) version of FrameUtil. The upstream file had AVX2
    // fast-paths for GetTileHalf8 / Sad / Sad64; each already carried an
    // equivalent scalar fallback (or a commented scalar reference), so this
    // keeps those bit-identical while dropping the x86-only dependency. Runs on
    // arm64. See mobipeg fastvideo/README for why (Rosetta lacks AVX2).
    public static class FrameUtil
    {
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static unsafe void GetTile8(byte[] data, int stride, int srcX, int srcY, byte[] result)
        {
            fixed (byte* dst = &result[0], src = &data[srcY * stride + srcX])
            {
                for (int y = 0; y < 8; y++)
                    ((ulong*)dst)[y] = *(ulong*)(src + y * stride);
            }
        }

        public static byte[] GetTile(byte[] data, int stride, int srcX, int srcY, int width, int height)
        {
            var result = new byte[height * width];
            for (int y = 0; y < height; y++)
                for (int x = 0; x < width; x++)
                    result[y * width + x] = data[(y + srcY) * stride + (x + srcX)];
            return result;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static void GetTile2x2Step2(byte[] data, int stride, int srcX, int srcY, byte[] dst)
        {
            dst[0] = data[(srcY) * stride + srcX];
            dst[1] = data[(srcY) * stride + srcX + 2];
            dst[2] = data[(srcY + 2) * stride + srcX];
            dst[3] = data[(srcY + 2) * stride + srcX + 2];
        }

        public static byte[] GetTile(byte[] data, int stride, int srcX, int srcY, int width, int height, int step)
        {
            var result = new byte[height * width];
            for (int y = 0; y < height; y++)
                for (int x = 0; x < width; x++)
                    result[y * width + x] = data[(y * step + srcY) * stride + x * step + srcX];
            return result;
        }

        // Averages two 5-bit-quantised samples exactly like the upstream scalar
        // fallback: q = (v>>3<<1) (+1 if non-zero); result = ((a+b)*16>>6)<<3.
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        private static int Half(int a, int b)
        {
            int qa = a >> 3 << 1;
            if (qa != 0) qa++;
            int qb = b >> 3 << 1;
            if (qb != 0) qb++;
            return (qa * 16 + qb * 16) >> 6 << 3;
        }

        // Half-resolution motion tile fetch with subpixel handling. Bit-identical
        // to the upstream scalar branches (the AVX2 fast-paths only covered the
        // in-bounds case and computed the same values).
        [MethodImpl(MethodImplOptions.AggressiveOptimization)]
        public static void GetTileHalf8(byte[] data, int width, int height, int srcX, int srcY, byte[] result)
        {
            int bx = srcX >> 1;
            int by = srcY >> 1;
            for (int y = 0; y < 8; y++)
            {
                int y1 = Math.Clamp(y + by, 0, height - 1);
                int y2 = Math.Clamp(y + by + 1, 0, height - 1);
                for (int x = 0; x < 8; x++)
                {
                    int x1 = Math.Clamp(x + bx, 0, width - 1);
                    int x2 = Math.Clamp(x + bx + 1, 0, width - 1);
                    int v;
                    if (((srcX | srcY) & 1) == 0)
                        v = data[y1 * width + x1];
                    else if ((srcY & 1) == 0)
                        v = Half(data[y1 * width + x1], data[y1 * width + x2]);
                    else if ((srcX & 1) == 0)
                        v = Half(data[y1 * width + x1], data[y2 * width + x1]);
                    else
                        v = Half(data[y1 * width + x1], data[y2 * width + x2]);
                    result[y * 8 + x] = (byte)v;
                }
            }
        }

        public static void SetTile(byte[,] data, int dstX, int dstY, int width, int height, byte[] src)
        {
            for (int y = 0; y < height; y++)
                for (int x = 0; x < width; x++)
                    data[y + dstY, x + dstX] = src[y * width + x];
        }

        public static void SetTile(byte[] data, int stride, int dstX, int dstY, int width, int height, int step,
            byte[] src)
        {
            for (int y = 0; y < height; y++)
                for (int x = 0; x < width; x++)
                    data[(y * step + dstY) * stride + x * step + dstX] = src[y * width + x];
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static void SetTile2x2Step2(byte[] data, int stride, int dstX, int dstY, byte[] src)
        {
            data[dstY * stride + dstX]           = src[0];
            data[dstY * stride + dstX + 2]       = src[1];
            data[(dstY + 2) * stride + dstX]     = src[2];
            data[(dstY + 2) * stride + dstX + 2] = src[3];
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static unsafe void SetTile8(byte[] data, int stride, int dstX, int dstY, byte[] src)
        {
            fixed (byte* pSrc = &src[0], pDst = &data[dstY * stride + dstX])
            {
                for (int y = 0; y < 8; y++)
                    *(ulong*)(pDst + y * stride) = ((ulong*)pSrc)[y];
            }
        }

        public static void SetTile(byte[] data, int stride, int dstX, int dstY, int width, int height, int step,
            byte[,] src)
        {
            for (int y = 0; y < height; y++)
                for (int x = 0; x < width; x++)
                    data[(y * step + dstY) * stride + x * step + dstX] = src[y, x];
        }

        public static unsafe byte[] GetBlockPixels16x16(byte[] Data, int X, int Y, int Stride, int Offset)
        {
            byte[] values = new byte[256];
            fixed (byte* pVals = &values[0])
            {
                ulong* pLVals = (ulong*)pVals;
                for (int y3 = 0; y3 < 16; y3++)
                {
                    fixed (byte* pData = &Data[(Y + y3) * Stride + X + Offset])
                    {
                        *pLVals++ = *((ulong*)pData);
                        *pLVals++ = *((ulong*)(pData + 8));
                    }
                }
            }

            return values;
        }

        public static unsafe byte[] GetBlockPixels8x8(byte[] Data, int X, int Y, int Stride, int Offset)
        {
            byte[] values = new byte[64];
            fixed (byte* pVals = &values[0], pData = &Data[Y * Stride + X + Offset])
            {
                ulong* pLVals = (ulong*)pVals;
                for (int y = 0; y < 8; y++)
                    *pLVals++ = *((ulong*)(pData + Stride * y));
            }

            return values;
        }

        public static unsafe byte[] GetBlockPixels4x4(byte[] Data, int X, int Y, int Stride, int Offset)
        {
            byte[] values = new byte[16];
            fixed (byte* pVals = &values[0], pData = &Data[Y * Stride + X + Offset])
            {
                uint* pLVals = (uint*)pVals;
                for (int y = 0; y < 4; y++)
                    *pLVals++ = *((uint*)(pData + Stride * y));
            }

            return values;
        }

        public static unsafe void SetBlockPixels4x4(byte[] Data, int X, int Y, int Stride, int Offset, byte[] Values)
        {
            fixed (byte* pVals = &Values[0], pData = &Data[Y * Stride + X + Offset])
            {
                uint* pLVals = (uint*)pVals;
                for (int y = 0; y < 4; y++)
                    *((uint*)(pData + Stride * y)) = *pLVals++;
            }
        }

        public static unsafe void SetBlockPixels8x8(byte[] Data, int X, int Y, int Stride, int Offset, byte[] Values)
        {
            fixed (byte* pVals = &Values[0], pData = &Data[Y * Stride + X + Offset])
            {
                ulong* pLVals = (ulong*)pVals;
                for (int y = 0; y < 8; y++)
                    *((ulong*)(pData + Stride * y)) = *pLVals++;
            }
        }

        public static int Sad64(ReadOnlySpan<byte> a, ReadOnlySpan<byte> b)
        {
            int result = 0;
            for (int i = 0; i < a.Length; i++)
                result += Math.Abs(a[i] - b[i]);
            return result;
        }

        public static ulong Sad(ReadOnlySpan<byte> a, ReadOnlySpan<byte> b)
        {
            ulong result = 0;
            for (int i = 0; i < a.Length; i++)
                result += (ulong)Math.Abs(a[i] - b[i]);
            return result;
        }
    }
}
