#!/usr/bin/env python3
"""moflex_addstreams — inject a second audio track and/or embedded subtitles into a .moflex.

The result stays a fully valid file for the OFFICIAL player (which plays the FIRST audio
stream and ignores the added ones) while our player exposes the extra track and subtitles:

  stream 0: video               (untouched)
  stream 1: seek index          (byte offsets patched -- blocks shift when streams are added)
  stream 2: primary audio       (untouched -- e.g. the English dub)
  stream 3: added audio         (--audio track.wav: 16-bit PCM mono/stereo, e.g. Japanese)
  stream 4: added subtitles     (--srt subs.srt: whole file as ONE data-stream frame at the
                                 head, same pattern the seek index uses)

Everything the official player validates is rebuilt: sync timestamp checksums, per-block
group continuity counters (each group keeps its ORIGINAL counter; groups just grow blocks),
and the seek index (same entry count, offsets remapped). Audio packets use the same framing
as native files: 4-byte per-channel IMA state headers, 256-sample subframes, 1024 samples
per packet, endframe timestamps relative to the enclosing sync group.

Usage:
  moflex_addstreams.py in.moflex out.moflex [options]

  --audio eng.wav      first audio stream (2) -- what the official player plays
  --audio2 jpn.wav     second audio stream (3) -- selectable in our player
  --srt subs.srt       embedded subtitles (stream 4)
  --strip-audio        DROP the file's existing audio first (we own both tracks;
                       the encode-pipeline audio is reference only)
  --audio-first        alternative to --strip-audio: keep the existing audio but move
                       it to stream 3 (bit patch); the --audio track takes stream 2
  --normalize          peak-normalize each provided WAV to -1 dBFS. NOTE this is PEAK, not
                       loudness: film mixes already peak near full scale, so it barely changes
                       how loud they SOUND (measured: -1.0 dB on Space Jam, i.e. quieter).
                       smoflex_build does EBU R128 loudness normalisation at extraction
                       instead; this pass then only takes up any headroom left.
  --trailer-audio x.wav  second language as a TRAILER after the last block (official player
                         never reads it; ours plays it as Audio Track 2)
  --trailer-srt x.srt    subtitles in the trailer; REPEATABLE for multiple languages
                         (lang tag from '.eng.srt'-style names; first listed = default)
  --nfo info.txt         library metadata (key=value lines: title, year, desc, genres,
                         category, runtime, date) -> 'NFO0' section; the player imports it
                         so the file needs NO scraping, even offline
  --art poster.jpg       poster art -> scaled via ffmpeg to the library's 132x188 RGB565
                         ('ART5' section); imported with the metadata

Hardware-validated design: the official player is STRICT -- any extra in-band stream hangs
it (data/unknown descriptors) or chokes its demux queue (extra audio: ~1fps, silent). So:
in-band audio is REPLACED (--strip-audio --audio eng.wav keeps the exact native layout) and
everything extra lives past the final block:

  [normal moflex blocks][sections...][u64 payload_off][8B magic "CSXTRA01"]
  section: [4cc][u32 len][data]
    'SUB0' -> the .srt bytes
    'AUD1' -> u32 rate, u16 channels, u16 samples/packet (1024), char lang[4] ("JPN"),
              then FIXED-SIZE ADPCM packets (in-band framing) -> seek = pure arithmetic
    'LNG0' -> char lang[4]: language of the IN-BAND audio track ("ENG")
  Languages auto-infer from ".xxx.wav" filenames; --lang / --trailer-lang override.
"""
import array
import struct
import sys
import wave

# ---------------- container primitives (shared knowledge with moflex_combine) ----------------

def sync_check(ts_bytes):
    crc = 0
    for b in ts_bytes:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x0001) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc ^ 0xAAAA

def rvarb(d, i):
    v = 0
    while True:
        b = d[i]; i += 1
        v = (v << 7) | (b & 0x7F)
        if not (b & 0x80):
            return v, i

class BitReader:
    def __init__(self, d, byte):
        self.d = d; self.start = byte; self.bp = 0
    def pop(self):
        b = self.d[self.start + (self.bp >> 3)]
        bit = (b >> (7 - (self.bp & 7))) & 1
        self.bp += 1
        return bit
    def popn(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | self.pop()
        return v
    def poplen(self):
        n = 1
        while self.pop() == 0:
            n += 1
        return n
    def after(self):
        return self.start + ((self.bp + 7) >> 3)

class BitWriter:
    def __init__(self):
        self.bits = []
    def put(self, v, n):
        for k in range(n - 1, -1, -1):
            self.bits.append((v >> k) & 1)
    def put_len(self, n):          # unary length as poplen reads it: (n-1) zeros then a 1
        self.bits.extend([0] * (n - 1)); self.bits.append(1)
    def bytes(self):
        out = bytearray((len(self.bits) + 7) // 8)
        for i, b in enumerate(self.bits):
            if b:
                out[i >> 3] |= 0x80 >> (i & 7)
        return bytes(out)

def chunk_header(si, endframe, efv, size, ef_bit=0):
    """Packet header preceding `size` payload bytes (payload starts at the next byte boundary)."""
    w = BitWriter()
    si_bits = max(1, si.bit_length())
    w.put_len(si_bits); w.put(si, si_bits)
    w.put(1 if endframe else 0, 1)
    if endframe:
        w.put_len(1); w.put(0, 1)        # X field = 0 (1 bit), as native packets carry
        w.put(ef_bit, 1)                 # audio uses 0; head data frames (index) use 1
        w.put_len(1)                     # b2 = 1 -> timestamp width 1*2+26 = 28 bits
        w.put(efv & ((1 << 28) - 1), 28)
    w.put(size - 1, 13)
    return w.bytes()

# ---------------- IMA-ADPCM encoder (mirror of decoder/adpcm_moflex.c) ----------------

STEP = [7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,
        107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,
        724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,
        3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,
        15289,16818,18500,20350,22385,24623,27086,29794,32767]
IDX = [-1,-1,-1,-1,2,4,6,8]

class ImaCh:
    __slots__ = ('pred', 'step')
    def __init__(self):
        self.pred = 0; self.step = 0

def ima_nibble(ch, sample):
    step = STEP[ch.step]
    diff = sample - ch.pred
    nib = 0
    if diff < 0:
        nib = 8; diff = -diff
    if diff >= step:
        nib |= 4; diff -= step
    if diff >= step >> 1:
        nib |= 2; diff -= step >> 1
    if diff >= step >> 2:
        nib |= 1
    # reconstruct exactly like the decoder
    d = ((2 * (nib & 7) + 1) * step) >> 3
    ch.pred += -d if (nib & 8) else d
    ch.pred = max(-32768, min(32767, ch.pred))
    ch.step += IDX[nib & 7]
    ch.step = max(0, min(88, ch.step))
    return nib

PKT_SAMPLES = 1024                     # per channel per packet (multiple of 256, like native)

def adpcm_packets(pcm, channels, rate):
    """[(start_us, payload_bytes)] for the whole track; encoder state carries across packets."""
    chs = [ImaCh() for _ in range(channels)]
    total = len(pcm) // channels
    out = []
    pos = 0
    while pos < total:
        if not (pos % (PKT_SAMPLES * 64)):              # ~1.5 s of audio between checks
            _ck()
        n = min(PKT_SAMPLES, total - pos)
        n -= n % 256                                   # subframes are exactly 256 samples
        if n <= 0:
            break
        payload = bytearray()
        for c in range(channels):                      # 4-byte state header per channel
            payload += struct.pack('<hh', chs[c].step, chs[c].pred)
        # careful: the header stores the state BEFORE encoding this packet's samples
        for sf in range(n // 256):
            base = pos + sf * 256
            for c in range(channels):
                for k in range(0, 256, 2):
                    lo = ima_nibble(chs[c], pcm[(base + k) * channels + c])
                    hi = ima_nibble(chs[c], pcm[(base + k + 1) * channels + c])
                    payload.append(lo | (hi << 4))
        out.append((pos * 1000000 // rate, bytes(payload)))
        pos += n
    return out

# ---------------- parse the source file into groups of packets ----------------

class Frame:
    __slots__ = ('si', 'payload', 'efbits', 'si_bits')
    def __init__(self, si, payload, efbits, si_bits):
        self.si = si; self.payload = payload
        self.efbits = efbits          # the raw bits between the endframe flag and the size field
        self.si_bits = si_bits        # unary width the source used for this stream index

class Group:
    __slots__ = ('ts', 'flags', 'frames', 'desc')
    def __init__(self, ts, flags, desc):
        self.ts = ts; self.flags = flags; self.frames = []; self.desc = desc

def parse(data):
    """Assemble the file into GROUPS of complete FRAMES (the original chunk boundaries are
    a property of block tiling, not of the content -- the packer re-splits to fill blocks
    exactly like the source muxer, which is what keeps a no-op repack byte-identical)."""
    n = len(data); pos = 0; size = None
    groups = []; desc = None; blocksize = None
    max_si = 0
    g = None
    acc = {}                                           # si -> (payload bytearray, group#)
    while pos + 2 <= n:
        if data[pos] == 0x4C and data[pos + 1] == 0x32:
            ts = int.from_bytes(data[pos + 4:pos + 12], 'big')
            size = int.from_bytes(data[pos + 12:pos + 14], 'big') + 1
            blocksize = size
            i = pos + 14
            dstart = i
            while True:
                estart = i
                t, i = rvarb(data, i); ss, i = rvarb(data, i); i += ss
                if t == 0:
                    dend = estart          # terminator excluded: pack() writes its own
                    break
            if desc is None:
                desc = bytes(data[dstart:dend])
            g = Group(ts, data[i], bytes(data[dstart:dend]))   # descriptors VARY per sync (tb quirk)
            groups.append(g)
            flags = data[i]; ci = i + 1
            if flags & 2:
                ci += 2
        else:
            flags = data[pos]; ci = pos + 1
            if flags & 2:
                ci += 2
        end = pos + size
        while ci < end and data[ci] != 0:
            br = BitReader(data, ci)
            si_bits = br.poplen(); si = br.popn(si_bits); ef = br.pop()
            efbits = []
            if ef:
                mark = br.bp
                b = br.poplen(); br.popn(b); br.pop()
                b2 = br.poplen(); br.popn(b2 * 2 + 26)
                efbits = [br.d[br.start + (k >> 3)] >> (7 - (k & 7)) & 1 for k in range(mark, br.bp)]
            pkt = br.popn(13) + 1
            po = br.after()
            payload = data[ci + (po - ci):po + pkt][:]  # payload bytes only
            if si not in acc:
                acc[si] = (bytearray(), len(groups) - 1)
            acc[si][0].extend(data[po:po + pkt])
            if ef:
                buf, gi0 = acc.pop(si)
                groups[gi0].frames.append(Frame(si, bytes(buf), efbits, si_bits))
            max_si = max(max_si, si)
            ci = po + pkt
        pos += size
    if pos != n:
        raise ValueError('block chain did not reach EOF — corrupt/unsupported')
    return groups, desc, blocksize, max_si

# ---------------- repack groups into a block stream ----------------

def make_efbits(efv, data_frame=0):
    """[X-len=1][X=0][bit][b2-len=1][28-bit efv] -- audio uses bit=0, head data frames bit=1."""
    bits = [1, 0, 1 if data_frame else 0, 1]
    for k in range(27, -1, -1):
        bits.append((efv >> k) & 1)
    return bits

def frame_chunk(si, si_bits, endframe, efbits, size):
    w = BitWriter()
    w.put_len(si_bits); w.put(si, si_bits)
    w.put(1 if endframe else 0, 1)
    if endframe:
        for b in efbits:
            w.bits.append(b)
    w.put(size - 1, 13)
    return w.bytes()

def pack(groups, extra_desc, blocksize):
    """Emit the block stream, re-splitting每 frame to FILL blocks exactly (source-muxer
    tiling). Returns (bytes, sync_offsets, payload positions per (group#, frame#))."""
    out = bytearray()
    sync_off = {}
    frame_pos = {}                                     # (g#, f#) -> [(file_off, nbytes), ...]
    for gi, g in enumerate(groups):
        if not (gi & 63):                              # every ~64 sync groups while repacking
            _ck()
        sync_off[gi] = len(out)
        tsb = g.ts.to_bytes(8, 'big')
        hdr = bytearray()
        hdr += b'\x4C\x32'
        hdr += sync_check(tsb).to_bytes(2, 'big')
        hdr += tsb
        hdr += (blocksize - 1).to_bytes(2, 'big')
        hdr += g.desc + extra_desc + b'\x00\x00'   # each sync keeps ITS OWN descriptor variant
        block = bytearray(hdr)
        block.append(g.flags)
        for fi, fr in enumerate(g.frames):
            sent = 0; spans = []
            hdr_fin  = len(frame_chunk(fr.si, fr.si_bits, 1, fr.efbits, 1))
            hdr_cont = len(frame_chunk(fr.si, fr.si_bits, 0, [], 1))
            while sent < len(fr.payload):
                remaining = len(fr.payload) - sent
                space = blocksize - len(block)
                if space >= hdr_fin + remaining and remaining <= 8192:
                    take, fin, h = remaining, 1, hdr_fin          # frame completes here
                elif space >= hdr_cont + 1 and remaining >= 2:
                    take = min(space - hdr_cont, remaining - 1, 8192)   # never finish on a cont
                    fin, h = 0, hdr_cont
                else:                                             # no useful room -> next block
                    block += b'\x00' * (blocksize - len(block))
                    out += block
                    block = bytearray(); block.append(g.flags)
                    continue
                ch = frame_chunk(fr.si, fr.si_bits, fin, fr.efbits if fin else [], take)
                spans.append((len(out) + len(block) + len(ch), take))
                block += ch + fr.payload[sent:sent + take]
                sent += take
            frame_pos[(gi, fi)] = spans
        if len(block) < blocksize:
            block += b'\x00' * (blocksize - len(block))
        out += block
    return bytes(out), sync_off, frame_pos

# ---------------- main ----------------

CANCEL = None          # optional callable set by the caller; truthy return aborts the run

class Aborted(Exception):
    """Raised out of the long loops when CANCEL() goes true, so a build can be stopped."""

def _ck():
    if CANCEL is not None and CANCEL():
        raise Aborted()

def main_args(args):
    """Same as the command line, but callable in-process:
       main_args(['in.moflex', 'out.moflex', '--audio', 'e.wav', ...])"""
    if len(args) < 2:
        print(__doc__); sys.exit(1)
    src, dst = args[0], args[1]
    wav_path = wav2_path = srt_path = None; audio_first = False; strip = False; norm = False
    subtype = 4; tr_wav = None; tr_srts = []; lang_main = lang_alt = None
    nfo_path = art_path = None; keep_tr_audio = False

    def infer_lang(path):
        import re as _re
        m = _re.search(r'\.([a-z]{2,3})\.(wav|flac)$', path or '', _re.I)
        return m.group(1).upper()[:3] if m else None

    def infer_lang_ext(path):
        import re as _re
        m = _re.search(r'\.([a-z]{2,3})\.(srt)$', path or '', _re.I)
        return m.group(1).upper()[:3] if m else None
    i = 2
    while i < len(args):
        if args[i] == '--audio': wav_path = args[i + 1]; i += 2
        elif args[i] == '--audio2': wav2_path = args[i + 1]; i += 2
        elif args[i] == '--srt': srt_path = args[i + 1]; i += 2
        elif args[i] == '--audio-first': audio_first = True; i += 1
        elif args[i] == '--strip-audio': strip = True; i += 1
        elif args[i] == '--normalize': norm = True; i += 1
        elif args[i] == '--subtype': subtype = int(args[i + 1]); i += 2
        elif args[i] == '--trailer-audio': tr_wav = args[i + 1]; i += 2
        elif args[i] == '--trailer-srt': tr_srts.append(args[i + 1]); i += 2
        elif args[i] == '--lang': lang_main = args[i + 1][:3].upper(); i += 2
        elif args[i] == '--trailer-lang': lang_alt = args[i + 1][:3].upper(); i += 2
        elif args[i] == '--nfo': nfo_path = args[i + 1]; i += 2
        elif args[i] == '--art': art_path = args[i + 1]; i += 2
        elif args[i] == '--keep-trailer-audio': keep_tr_audio = True; i += 1
        else: print('unknown arg', args[i]); sys.exit(1)

    data = open(src, 'rb').read()
    # A source that is ALREADY a SUPER MOFLEX carries a CSXTRA payload past its last block, so
    # the block walk would stop short of EOF and call the file corrupt. Cut the payload off,
    # keep it, and rebuild the trailer from the new arguments.
    old_payload = b''
    old_sections = {}
    if len(data) > 16 and data[-8:] == b'CSXTRA01':
        poff = struct.unpack('<Q', data[-16:-8])[0]
        if 0 < poff <= len(data) - 16:
            old_payload = data[poff:-16]
            data = data[:poff]
            q = 0                                   # index it: 4cc -> whole section bytes
            while q + 8 <= len(old_payload):
                cc, ln = struct.unpack_from('<4sI', old_payload, q)
                if ln == 0 or q + 8 + ln > len(old_payload):
                    break
                old_sections.setdefault(cc, old_payload[q:q + 8 + ln])
                q += 8 + ln
            print(f'  source already has a trailer ({len(old_payload)} B, '
                  f'{"+".join(c.decode() for c in old_sections)}) — rebuilding it')
    groups, desc, blocksize, max_si = parse(data)
    print(f'  source: {len(data)} B, {len(groups)} sync groups, block {blocksize}, streams<= {max_si}')

    new_desc = bytearray()                 # ADDED descriptor entries (appended to every sync's own)
    next_si = max_si + 1

    if strip:
        # remove the existing audio: its frames AND its descriptor entry from EVERY sync
        audio_sis = set()
        for g in groups:
            nd = bytearray(); j = 0
            while j < len(g.desc):
                t, j2 = rvarb(g.desc, j); ss, j3 = rvarb(g.desc, j2)
                if t == 2:
                    audio_sis.add(g.desc[j3])
                    j = j3 + ss
                    continue
                nd += g.desc[j:j3 + ss]
                j = j3 + ss
            g.desc = bytes(nd)
        dropped = 0
        for g in groups:
            kept = [fr for fr in g.frames if fr.si not in audio_sis]
            dropped += len(g.frames) - len(kept)
            g.frames = kept
        print(f'  stripped existing audio: streams {sorted(audio_sis)}, {dropped} frames removed')
        if audio_sis:
            next_si = min(audio_sis)   # replacement takes the freed slot: layout stays native

    def load_wav(path):
        # array('h'), not a list: a feature film is ~200 M samples, and as Python int objects
        # in a list that is several GB and gets the repack OOM-killed. Two bytes a sample here,
        # and indexing still hands back plain ints, so nothing downstream changes.
        w = wave.open(path, 'rb')
        assert w.getsampwidth() == 2, 'need 16-bit PCM'
        chn, rate = w.getnchannels(), w.getframerate()
        pcm = array.array('h')
        while True:                                     # chunked: never hold a second full copy
            b = w.readframes(1 << 20)
            if not b:
                break
            pcm.frombytes(b)
        w.close()
        if sys.byteorder == 'big':
            pcm.byteswap()
        if norm:
            peak = max(1, max(pcm), -min(pcm))
            target = 29204                              # -1.0 dBFS: scale=target/peak, clip-proof
            if peak != target:
                sc = target / peak
                for i in range(len(pcm)):               # in place, for the same reason
                    pcm[i] = int(pcm[i] * sc)
                db = __import__('math').log10(sc) * 20
                print(f'    normalize {path.split("/")[-1]}: peak {peak} -> {target} ({db:+.1f} dB)')
        return pcm, chn, rate

    def interleave(apkts, asi, rate):
        # bucket per group, weave among existing frames (audio alternates with video natively)
        spans = [g.ts for g in groups]
        dur_pkt = PKT_SAMPLES * 1000000 // rate
        per_group = [[] for _ in groups]
        gi = 0
        for (t_us, payload) in apkts:
            t_end = t_us + dur_pkt
            while gi + 1 < len(groups) and spans[gi + 1] <= t_end + 1:
                gi += 1
            efv = max(0, t_end - (spans[gi] - 1))
            per_group[gi].append(Frame(asi, payload, make_efbits(efv, 0), max(1, asi.bit_length())))
        for gi2, add in enumerate(per_group):
            if not add:
                continue
            old = groups[gi2].frames
            if not old:
                groups[gi2].frames = add
                continue
            step = max(1, len(old) // (len(add) + 1))
            woven = []; ai = 0
            for k, fr in enumerate(old):
                if ai < len(add) and k % step == 0:
                    woven.append(add[ai]); ai += 1
                woven.append(fr)
            woven.extend(add[ai:])
            groups[gi2].frames = woven

    if wav_path:
        if audio_first and max_si == 2 and not strip:
            # existing single audio (si=2) moves to si=3; the new track takes 2 (same bit width)
            for g in groups:
                for fr in g.frames:
                    if fr.si == 2:
                        fr.si = 3
            for g in groups:
                nd = bytearray(); j = 0
                while j < len(g.desc):
                    t, j2 = rvarb(g.desc, j); ss, j3 = rvarb(g.desc, j2)
                    entry = bytearray(g.desc[j:j3 + ss])
                    if t == 2 and entry[j3 - j] == 2:
                        entry[j3 - j] = 3
                    nd += entry
                    j = j3 + ss
                g.desc = bytes(nd)
            asi = 2; next_si = 4
        else:
            asi = next_si; next_si += 1
        pcm, chn, rate = load_wav(wav_path)
        apkts = adpcm_packets(pcm, chn, rate)
        new_desc += bytes([0x02, 0x06, asi, 0x01]) + (rate - 1).to_bytes(3, 'big') + bytes([chn - 1])
        print(f'  audio 1: {len(pcm)//chn} samples @{rate}Hz x{chn} -> {len(apkts)} packets as stream {asi}')
        interleave(apkts, asi, rate)

    if wav2_path:
        pcm2, chn2, rate2 = load_wav(wav2_path)
        apkts2 = adpcm_packets(pcm2, chn2, rate2)
        asi2 = next_si; next_si += 1
        new_desc += bytes([0x02, 0x06, asi2, 0x01]) + (rate2 - 1).to_bytes(3, 'big') + bytes([chn2 - 1])
        print(f'  audio 2: {len(pcm2)//chn2} samples @{rate2}Hz x{chn2} -> {len(apkts2)} packets as stream {asi2}')
        interleave(apkts2, asi2, rate2)

    # locate the seek index frame (stream 1) BEFORE any subtitle insertion
    idx_ref = None
    for gi, g in enumerate(groups):
        for fi, fr in enumerate(g.frames):
            if fr.si == 1:
                idx_ref = (gi, fi)
                break
        if idx_ref:
            break

    if srt_path:
        srt = open(srt_path, 'rb').read()
        ssi = next_si; next_si += 1
        new_desc += bytes([subtype, 0x02, ssi, 0x00])
        fr = Frame(ssi, srt, make_efbits(1, 1), max(1, ssi.bit_length()))
        if idx_ref:                                     # one frame, right after the seek index
            groups[idx_ref[0]].frames.insert(idx_ref[1] + 1, fr)
        else:
            groups[0].frames.insert(0, fr)
        print(f'  subtitles: {len(srt)} B as ONE frame on stream {ssi} (type {subtype})')

    out, sync_off, frame_pos = pack(groups, bytes(new_desc), blocksize)
    out = bytearray(out)

    # patch the seek index offsets for the new block positions
    if idx_ref:
        blob = bytearray(groups[idx_ref[0]].frames[idx_ref[1] if not (srt_path and False) else idx_ref[1]].payload)
        # (the index frame object is unchanged by insertion AFTER it, but its position may have
        #  shifted if audio frames were woven before it -- find it again by stream)
        for fi, fr in enumerate(groups[idx_ref[0]].frames):
            if fr.si == 1:
                idx_ref = (idx_ref[0], fi); blob = bytearray(fr.payload)
                break
        cnt, frames_total = struct.unpack('<II', blob[0:8])
        old_syncs = {}
        pos = 0; size = None; gnum = -1
        while pos + 2 <= len(data):
            if data[pos] == 0x4C and data[pos + 1] == 0x32:
                size = int.from_bytes(data[pos + 12:pos + 14], 'big') + 1
                gnum += 1
                old_syncs[pos] = gnum
            pos += size
        patched = 0
        for k in range(cnt):
            o = 16 + 24 * k
            f_, t_, off_ = struct.unpack('<QQQ', blob[o:o + 24])
            if off_ in old_syncs:
                blob[o + 16:o + 24] = struct.pack('<Q', sync_off[old_syncs[off_]])
                patched += 1
        w = 0
        for (foff, nbytes) in frame_pos[idx_ref]:
            out[foff:foff + nbytes] = blob[w:w + nbytes]
            w += nbytes
        print(f'  seek index: {patched}/{cnt} offsets remapped')

    if tr_wav or tr_srts or nfo_path or art_path or (keep_tr_audio and old_payload):
        payload = bytearray()
        if nfo_path:
            nfo = open(nfo_path, 'rb').read()
            payload += struct.pack('<4sI', b'NFO0', len(nfo)) + nfo
            print(f'  library info: {len(nfo)} B')
        if art_path:
            import subprocess, tempfile, os as _os
            tmp = tempfile.mktemp(suffix='.raw')
            subprocess.run(['ffmpeg', '-y', '-v', 'error', '-i', art_path,
                            '-vf', 'scale=132:188', '-pix_fmt', 'rgb565le', '-f', 'rawvideo', tmp],
                           check=True)
            raw = open(tmp, 'rb').read(); _os.unlink(tmp)
            assert len(raw) == 132 * 188 * 2, len(raw)
            payload += struct.pack('<4sIHH', b'ART5', 4 + len(raw), 132, 188) + raw
            print(f'  poster art: {art_path.split("/")[-1]} -> 132x188 RGB565')
        lm = lang_main or infer_lang(wav_path)
        if lm:
            payload += struct.pack('<4sI4s', b'LNG0', 4, lm.encode()[:3].ljust(4, b'\0'))
            print(f'  in-band language tag: {lm}')
        elif b'LNG0' in old_sections:      # not re-tagging the in-band track: keep its label
            payload += old_sections[b'LNG0']
            print(f"  in-band language tag kept: "
                  f"{old_sections[b'LNG0'][8:12].rstrip(bytes([0])).decode(errors='replace')}")
        for ts_path in tr_srts:                       # repeatable: one SUB1 per language
            srt = open(ts_path, 'rb').read()
            sl = (infer_lang_ext(ts_path) or 'SUB').encode()[:3].ljust(4, b'\0')
            payload += struct.pack('<4sI4s', b'SUB1', 4 + len(srt), sl) + srt
            print(f'  trailer subs [{sl.rstrip(chr(0).encode()).decode()}]: {len(srt)} B')
        if tr_wav:
            pcm, chn, rate = load_wav(tr_wav)
            total = len(pcm) // chn
            pad = (-total) % PKT_SAMPLES                 # pad tail with silence to a full packet
            if pad:
                pcm.extend(array.array('h', bytes(pad * chn * 2)))
            apkts = adpcm_packets(pcm, chn, rate)
            blob = b''.join(pl for _, pl in apkts)
            la = (lang_alt or infer_lang(tr_wav) or 'ALT').encode()[:3].ljust(4, b'\0')
            payload += struct.pack('<4sIIHH4s', b'AUD1', 12 + len(blob), rate, chn, PKT_SAMPLES, la) + blob
            print(f'  trailer audio [{la.rstrip(b"\0").decode()}]: {len(apkts)} packets @{rate}Hz x{chn} ({len(blob)} B)')
        elif keep_tr_audio and old_sections:
            # rebuilding the trailer of a file that already had one, with no new second track
            # supplied: carry the old audio section over untouched rather than losing it
            for cc in (b'AUD1', b'AUD0'):
                if cc in old_sections:
                    sec = old_sections[cc]
                    payload += sec
                    # AUD1 body: u32 rate, u16 chn, u16 pktsamp, char lang[4]
                    lang = sec[16:20].rstrip(b'\0').decode(errors='replace') if cc == b'AUD1' else '?'
                    print(f'  trailer audio kept from the source [{lang}]: {len(sec) - 8} B')
                    break
        base = len(out)
        out += payload + struct.pack('<Q8s', base, b'CSXTRA01')
    open(dst, 'wb').write(out)
    print(f'wrote {dst}: {len(out)} B ({len(out)-len(data):+} B)')

def main():
    main_args(sys.argv[1:])

if __name__ == '__main__':
    main()
