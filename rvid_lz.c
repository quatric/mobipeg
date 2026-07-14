/*
 * Nintendo LZ10 (0x10) compressor for RocketVideo (.rvid) frames.
 *
 * Direct port of Vid2RVID's lz77.cpp (originally from Gericom's
 * EFE/EveryFileExplorer) exposed as a C entry point so rvid.py can call it via
 * ctypes.  The RocketVideoPlayer decodes frames with libnds `decompress(..,
 * LZ77)`, which is exactly this LZ10 variant: 4-byte header (0x10 + 24-bit
 * decompressed size), then flag-byte groups of 8 tokens, each token either a
 * literal or a (length 3..18, distance 1..4096) back-reference.
 *
 * Build (done on demand by rvid.py, result cached next to the sources):
 *   cc -O3 -shared -fPIC rvid_lz.c -o rvid_lz.so
 */
#include <string.h>
#include <stddef.h>

/*
 * Compress `dataSize` bytes of `data` into `out` (capacity `outCap`).
 * Returns the compressed length (padded to a multiple of 4, matching the
 * reference), or -1 if the output buffer is too small.
 */
int rvid_lz10(const unsigned char *data, int dataSize,
              unsigned char *out, int outCap)
{
    if (outCap < 4)
        return -1;

    const unsigned char *dataptr = data;
    unsigned char *resultptr = out;
    int dstoffs = 4;

    *resultptr++ = 0x10;
    *resultptr++ = (unsigned char)(dataSize & 0xFF);
    *resultptr++ = (unsigned char)((dataSize >> 8) & 0xFF);
    *resultptr++ = (unsigned char)((dataSize >> 16) & 0xFF);

    int length = dataSize;
    int Offs = 0;

    while (1) {
        if (dstoffs >= outCap)
            return -1;
        int headeroffs = dstoffs++;
        resultptr++;               /* reserve the flag byte */
        unsigned char header = 0;

        for (int i = 0; i < 8; i++) {
            int comp = 0;
            int back = 1;
            int nr = 2;
            {
                const unsigned char *ptr = dataptr - 1;
                int maxnum = 18;
                if (length - Offs < maxnum) maxnum = length - Offs;
                int maxback = 0x1000;
                if (Offs < maxback) maxback = Offs;
                const unsigned char *minptr = dataptr - maxback;
                int tmpnr;
                while (minptr <= ptr) {
                    if (ptr[0] == dataptr[0] && ptr[1] == dataptr[1] &&
                        ptr[2] == dataptr[2]) {
                        tmpnr = 3;
                        while (tmpnr < maxnum && ptr[tmpnr] == dataptr[tmpnr])
                            tmpnr++;
                        if (tmpnr > nr) {
                            if (Offs + tmpnr > length) {
                                nr = length - Offs;
                                back = (int)(dataptr - ptr);
                                break;
                            }
                            nr = tmpnr;
                            back = (int)(dataptr - ptr);
                            if (nr == maxnum) break;
                        }
                    }
                    --ptr;
                }
            }

            if (nr > 2) {
                if (dstoffs + 2 > outCap) return -1;
                Offs += nr;
                dataptr += nr;
                *resultptr++ = (unsigned char)((((back - 1) >> 8) & 0xF) |
                                               (((nr - 3) & 0xF) << 4));
                *resultptr++ = (unsigned char)((back - 1) & 0xFF);
                dstoffs += 2;
                comp = 1;
            } else {
                if (dstoffs + 1 > outCap) return -1;
                *resultptr++ = *dataptr++;
                dstoffs++;
                Offs++;
            }

            header = (unsigned char)((header << 1) | (comp & 1));
            if (Offs >= length) {
                header = (unsigned char)(header << (7 - i));
                break;
            }
        }

        out[headeroffs] = header;
        if (Offs >= length) break;
    }

    while ((dstoffs % 4) != 0) {
        if (dstoffs >= outCap) return -1;
        out[dstoffs++] = 0;
    }
    return dstoffs;
}
