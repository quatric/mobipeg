using System;
using System.Collections.Generic;
using System.IO;
using Gericom.FastVideoDS;
using Gericom.FastVideoDS.Frames;
using Gericom.FastVideoDSEncoder; // Adpcm, SpanExtensions
using FvEnc = Gericom.FastVideoDS.FastVideoDSEncoder;

namespace Mobipeg.FastVideo
{
    // Raw-I/O FastVideoDS encoder CLI for mobipeg.
    //
    //   fvenc --height H --fps-num N --fps-den D --audio-rate R --frames F \
    //         [--q 30] [--gop 250] --raw video.bgra [--pcm audio.s16] out.fv
    //
    // video.bgra : raw BGRA frames, 256 x H, F frames (from ffmpeg -pix_fmt bgra)
    // audio.s16  : raw interleaved s16 stereo PCM at R Hz (ffmpeg -f s16le -ac 2)
    //
    // The .fv container it writes matches the reference FvEncoder byte-for-byte:
    //   header: "FVDS", u16 width(256), u16 height, u32 fpsNum, u32 fpsDen,
    //           u16 audioRate, u16 channels(2), u32 frameCount, u32 keyFrameCount,
    //           then keyFrameCount * (u32 frame, u32 fileOffset)
    //   body:   per frame  u32 sizeField = (len & 0x1FFFF) | (audioFrames << 17),
    //           frame data padded to 4, then audioFrames * (L then R) ADPCM blocks
    //           of 4 + 128 bytes (256 samples) each.
    internal static class Program
    {
        private const int AudioFrameSize = 256;

        private static int Main(string[] args)
        {
            string raw = null, pcm = null, outPath = null;
            int height = 0, fpsNum = 0, fpsDen = 1, audioRate = 0, frames = 0, q = 30, gop = 250;

            for (int i = 0; i < args.Length; i++)
            {
                switch (args[i])
                {
                    case "--height":     height    = int.Parse(args[++i]); break;
                    case "--fps-num":    fpsNum    = int.Parse(args[++i]); break;
                    case "--fps-den":    fpsDen    = int.Parse(args[++i]); break;
                    case "--audio-rate": audioRate = int.Parse(args[++i]); break;
                    case "--frames":     frames    = int.Parse(args[++i]); break;
                    case "--q":          q         = int.Parse(args[++i]); break;
                    case "--gop":        gop       = int.Parse(args[++i]); break;
                    case "--raw":        raw       = args[++i]; break;
                    case "--pcm":        pcm       = args[++i]; break;
                    default:             outPath   = args[i]; break;
                }
            }

            if (raw == null || outPath == null || height <= 0 || frames <= 0 || fpsNum <= 0)
            {
                Console.Error.WriteLine(
                    "usage: fvenc --height H --fps-num N --fps-den D --frames F " +
                    "[--audio-rate R --pcm a.s16] [--q 30] [--gop 250] --raw v.bgra out.fv");
                return 2;
            }

            const int width  = 256;
            int frameBytes    = width * height * 4;

            // --- video pass: encode into a temp body file, collect keyframes --- //
            string bodyPath = outPath + ".body";
            var keyFrames   = new List<(int frame, long offset)>();

            // audio source, read in 256-sample stereo blocks on demand
            using var pcmStream = pcm != null ? File.OpenRead(pcm) : null;
            var pcmBuf = new byte[AudioFrameSize * 2 * 2]; // 256 samples * 2ch * 2 bytes
            var blockL = new short[AudioFrameSize];
            var blockR = new short[AudioFrameSize];
            Adpcm.AdpcmState stateL = null, stateR = null;

            using (var vin = File.OpenRead(raw))
            using (var body = File.Create(bodyPath))
            {
                var pool    = new FramePool(width, height);
                var encoder = new FvEnc(width, height, q, 0, gop);
                var frameBuf = new byte[frameBytes];

                int inFrame = 0, outFrame = 0;
                while (true)
                {
                    if (inFrame < frames)
                    {
                        var rf = ReadFrame(vin, frameBuf, pool, width);
                        if (rf != null)
                        {
                            encoder.SendFrame(rf);
                            if (++inFrame == frames)
                                encoder.Flush();
                        }
                        else
                        {
                            inFrame = frames;
                            encoder.Flush();
                        }
                    }

                    var enc = encoder.ReceiveFrame();
                    if (enc != null)
                    {
                        // audio sample accounting, identical to the reference muxer
                        long expected = (long)audioRate * (outFrame + 1) * fpsDen / fpsNum;
                        long written  = ((long)audioRate * outFrame * fpsDen / fpsNum) /
                                        AudioFrameSize * AudioFrameSize;
                        int newSamples  = (int)(expected - written);
                        int audioFrames = newSamples > 0 ? newSamples / AudioFrameSize : 0;

                        long curPos  = body.Position;
                        int  frameLen = (enc.Data.Length + 3) & ~3;
                        uint sizeField = ((uint)frameLen & 0x1FFFFu) | ((uint)audioFrames << 17);

                        Span<byte> sf = stackalloc byte[4];
                        sf[0] = (byte)(sizeField & 0xFF);
                        sf[1] = (byte)((sizeField >> 8) & 0xFF);
                        sf[2] = (byte)((sizeField >> 16) & 0xFF);
                        sf[3] = (byte)((sizeField >> 24) & 0xFF);
                        body.Write(sf);
                        body.Write(enc.Data, 0, enc.Data.Length);
                        for (int p = enc.Data.Length; p < frameLen; p++)
                            body.WriteByte(0);

                        // interleaved ADPCM audio for this video frame (L then R)
                        for (int j = 0; j < audioFrames; j++)
                        {
                            ReadAudioBlock(pcmStream, pcmBuf, blockL, blockR);
                            (var dataL, stateL) = Adpcm.Encode(blockL, stateL, true);
                            body.Write(dataL, 0, dataL.Length);
                            (var dataR, stateR) = Adpcm.Encode(blockR, stateR, true);
                            body.Write(dataR, 0, dataR.Length);
                        }

                        if (enc.Type == FvEnc.FvFrameType.IFrame)
                            keyFrames.Add((outFrame, curPos));

                        // Note: enc.DecFrame is the encoder's own reference frame;
                        // its lifetime is managed internally — do not dispose it.
                        outFrame++;
                    }

                    if (inFrame == frames && encoder.FrameQueueEmpty)
                        break;
                }

                Console.Error.WriteLine($"fvenc: encoded {outFrame} frames, {keyFrames.Count} keyframes");
            }

            // --- assemble final file: header + body --- //
            int headerLen = 0x1C + keyFrames.Count * 8;
            var header = new byte[headerLen];
            var hs = header.AsSpan();
            header[0] = (byte)'F'; header[1] = (byte)'V'; header[2] = (byte)'D'; header[3] = (byte)'S';
            hs.WriteLe<ushort>(0x04, (ushort)width);
            hs.WriteLe<ushort>(0x06, (ushort)height);
            hs.WriteLe<uint>(0x08, (uint)fpsNum);
            hs.WriteLe<uint>(0x0C, (uint)fpsDen);
            hs.WriteLe<ushort>(0x10, (ushort)audioRate);
            hs.WriteLe<ushort>(0x12, 2);
            hs.WriteLe<uint>(0x14, (uint)frames);
            hs.WriteLe<uint>(0x18, (uint)keyFrames.Count);
            int ho = 0x1C;
            foreach (var (frame, offset) in keyFrames)
            {
                hs.WriteLe<uint>(ho, (uint)frame);
                hs.WriteLe<uint>(ho + 4, (uint)(headerLen + offset));
                ho += 8;
            }

            using (var outStream = File.Create(outPath))
            {
                outStream.Write(header, 0, header.Length);
                using (var body = File.OpenRead(bodyPath))
                    body.CopyTo(outStream);
            }
            File.Delete(bodyPath);

            Console.Error.WriteLine($"fvenc: wrote {outPath}");
            return 0;
        }

        private static unsafe RefFrame ReadFrame(Stream vin, byte[] buf, FramePool pool, int width)
        {
            int read = 0;
            while (read < buf.Length)
            {
                int n = vin.Read(buf, read, buf.Length - read);
                if (n == 0)
                    return null; // clean EOF (no partial frames expected)
                read += n;
            }

            var rf = pool.AcquireFrame();
            fixed (byte* p = &buf[0])
                rf.Frame.FromRgba32(p, width * 4);
            return rf;
        }

        // Reads one 256-sample stereo block; zero-fills past end of PCM (silence).
        private static void ReadAudioBlock(Stream pcm, byte[] buf, short[] left, short[] right)
        {
            int read = 0;
            if (pcm != null)
            {
                while (read < buf.Length)
                {
                    int n = pcm.Read(buf, read, buf.Length - read);
                    if (n == 0) break;
                    read += n;
                }
            }
            for (int i = read; i < buf.Length; i++)
                buf[i] = 0;
            for (int i = 0; i < AudioFrameSize; i++)
            {
                left[i]  = (short)(buf[i * 4] | (buf[i * 4 + 1] << 8));
                right[i] = (short)(buf[i * 4 + 2] | (buf[i * 4 + 3] << 8));
            }
        }
    }
}
