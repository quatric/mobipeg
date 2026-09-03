#!/usr/bin/env python3
"""smoflex_build — the SUPER MOFLEX build pipeline (importable; used by the GUI and CLI).

Turns (source video + encoded .moflex + options) into a self-contained SUPER MOFLEX:
  * English audio in-band (what the OFFICIAL 3DS player plays)
  * original-language audio + every subtitle in a trailer only our player reads
  * optional TMDB metadata + poster embedded so the library needs no scraping

Requires ffmpeg / ffprobe on PATH. Pure-Python otherwise (Tkinter GUI ships with Python).
"""
MAX_CUT = -1.0        # never attenuate a master by more than this (dB)
import contextlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import urllib.request
import wave

import smoflex_core as core

# ---- external tools (ffmpeg/ffprobe) -------------------------------------------------------
# Windows users rarely have ffmpeg on PATH, so we also look beside the app and in the usual
# install spots, and the GUI lets them point at it by hand.

FFMPEG = 'ffmpeg'
FFPROBE = 'ffprobe'
EXE = '.exe' if sys.platform == 'win32' else ''
# no flashing console windows for every child process on Windows
_NOWIN = {'creationflags': 0x08000000} if sys.platform == 'win32' else {}

def _search_dirs():
    here = os.path.dirname(os.path.abspath(__file__))
    return [here, os.path.join(here, 'ffmpeg'), os.path.join(here, 'ffmpeg', 'bin'),
            '/opt/homebrew/bin', '/usr/local/bin', '/usr/bin', '/opt/local/bin',
            r'C:\Program Files\ffmpeg\bin', r'C:\ffmpeg\bin']

def find_tools():
    """Locate ffmpeg/ffprobe. Returns (ffmpeg, ffprobe); either may be None."""
    found = []
    for name in ('ffmpeg', 'ffprobe'):
        p = shutil.which(name)
        if not p:
            for d in _search_dirs():
                c = os.path.join(d, name + EXE)
                if os.path.isfile(c):
                    p = c
                    break
        found.append(p)
    return tuple(found)

def set_tools(ffmpeg=None, ffprobe=None):
    """Pin the binaries (GUI 'Locate ffmpeg…'). Passing the ffmpeg path alone finds ffprobe
    next to it, which is how every ffmpeg distribution ships."""
    global FFMPEG, FFPROBE
    if ffmpeg:
        FFMPEG = ffmpeg
        if not ffprobe:
            c = os.path.join(os.path.dirname(ffmpeg), 'ffprobe' + EXE)
            if os.path.isfile(c):
                FFPROBE = c
    if ffprobe:
        FFPROBE = ffprobe

def tools_ready():
    for exe in (FFMPEG, FFPROBE):
        if not (shutil.which(exe) or os.path.isfile(exe)):
            return False
    return True

class Cancelled(Exception):
    """Raised when the caller's cancel flag is set; the GUI turns it into 'Cancelled'."""

def _run(cmd, cancel=None, capture=False):
    """subprocess.run with a cancel flag (threading.Event). Kills the child on cancel."""
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
                         stderr=subprocess.PIPE, text=True, **_NOWIN)
    while True:
        try:
            out, err = p.communicate(timeout=0.25)
            break
        except subprocess.TimeoutExpired:
            if cancel is not None and cancel.is_set():
                p.kill()
                p.communicate()
                raise Cancelled()
    return p.returncode, (out or ''), (err or '')

class _LogStream:
    """File-like sink so the core builder's print() output lands in the GUI log.

    Guards against a log callback that prints: stdout is redirected here while the core runs,
    so log -> print -> write -> log would recurse until the stack blew up."""
    def __init__(self, log):
        self.log = log
        self.buf = ''
        self._busy = False
    def write(self, s):
        self.buf += s
        while '\n' in self.buf:
            line, self.buf = self.buf.split('\n', 1)
            if line.strip() and not self._busy:
                self._busy = True
                try:
                    self.log(line.rstrip())
                finally:
                    self._busy = False
        return len(s)
    def flush(self):
        if self.buf.strip() and not self._busy:
            self._busy = True
            try:
                self.log(self.buf.rstrip())
            finally:
                self._busy = False
        self.buf = ''

# ---- language code helpers ----------------------------------------------------------------

LANG3 = {          # 2-letter / loose -> 3-letter code used on the button + CC cycler
    'en': 'ENG', 'eng': 'ENG', 'ja': 'JPN', 'jpn': 'JPN', 'jp': 'JPN',
    'fr': 'FRE', 'fre': 'FRE', 'de': 'GER', 'ger': 'GER', 'it': 'ITA', 'ita': 'ITA',
    'es': 'SPA', 'spa': 'SPA', 'es-419': 'LAT', '419': 'LAT', 'pt': 'POR', 'por': 'POR',
    'ru': 'RUS', 'rus': 'RUS', 'ko': 'KOR', 'kor': 'KOR', 'zh': 'CHI', 'chi': 'CHI',
    'tr': 'TUR', 'tur': 'TUR', 'pl': 'POL', 'pol': 'POL', 'el': 'GRE', 'gre': 'GRE',
    'fi': 'FIN', 'fin': 'FIN', 'tl': 'TGL', 'tgl': 'TGL', 'nl': 'DUT', 'sv': 'SWE',
    'ar': 'ARA', 'he': 'HEB', 'iw': 'HEB', 'cs': 'CZE', 'da': 'DAN', 'no': 'NOR',
    'nb': 'NOR', 'hu': 'HUN', 'ro': 'RUM', 'th': 'THA', 'vi': 'VIE', 'id': 'IND',
    'hi': 'HIN', 'uk': 'UKR', 'sk': 'SLO', 'bg': 'BUL', 'sr': 'SCC', 'hr': 'SCR',
    'ms': 'MAY', 'fa': 'PER', 'bn': 'BEN', 'ta': 'TAM', 'te': 'TEL', 'ml': 'MAL',
    'pt-br': 'POR', 'zh-cn': 'CHI', 'zh-tw': 'CHI', 'es-mx': 'LAT',
}
def to3(code):
    return LANG3.get(code.lower(), code.upper()[:3])

LANGNAMES = {           # subtitle sites label files by name as often as by code
    'english': 'ENG', 'japanese': 'JPN', 'french': 'FRE', 'german': 'GER', 'spanish': 'SPA',
    'castilian': 'CAS', 'italian': 'ITA', 'portuguese': 'POR', 'brazilian': 'POR',
    'russian': 'RUS', 'korean': 'KOR', 'chinese': 'CHI', 'mandarin': 'CHI', 'turkish': 'TUR',
    'polish': 'POL', 'greek': 'GRE', 'finnish': 'FIN', 'tagalog': 'TGL', 'filipino': 'TGL',
    'dutch': 'DUT', 'swedish': 'SWE', 'norwegian': 'NOR', 'danish': 'DAN', 'czech': 'CZE',
    'hungarian': 'HUN', 'romanian': 'RUM', 'arabic': 'ARA', 'hebrew': 'HEB', 'thai': 'THA',
    'vietnamese': 'VIE', 'indonesian': 'IND', 'hindi': 'HIN', 'ukrainian': 'UKR',
    'latin': 'LAT', 'sdh': 'SDH', 'signs': 'SGN', 'forced': 'SGN',
}

# ---- what the player can actually draw -----------------------------------------------------
# Kept in step with the player's glyph tables (playback/font8x8_ext.h, playback/font16.h).
# 8x8 covers the scripts that fit an 8-pixel cell; 16x16 covers the ones that cannot possibly
# (Hangul stacks three jamo, kanji carry far more strokes than 64 pixels hold).

def _fits8(cp):
    return (0x20 <= cp <= 0x7E or 0xA0 <= cp <= 0xFF        # ASCII + Latin-1
            or 0x100 <= cp <= 0x17F                          # Latin Extended-A (incl. Turkish)
            or 0x386 <= cp <= 0x3CE                          # Greek
            or 0x400 <= cp <= 0x45F                          # Cyrillic
            or 0x590 <= cp <= 0x5FF                          # Hebrew
            or 0x2010 <= cp <= 0x203A or cp in (0x20AC, 0x266A, 0x266B))

def _jis_level1(cp):
    b = chr(cp).encode('shift_jis', 'ignore')                # the common kanji, not level 2
    return bool(b) and len(b) == 2 and 0x88 <= b[0] <= 0x98

def _ks_x_1001(cp):
    b = chr(cp).encode('cp949', 'ignore')                    # the standard Korean syllable set
    return len(b) == 2 and 0xB0 <= b[0] <= 0xC8 and b[1] >= 0xA1

def _fits16(cp):
    if 0x3000 <= cp <= 0x30FF:                               # kana + CJK punctuation
        return True
    if 0xAC00 <= cp <= 0xD7A3:
        return _ks_x_1001(cp)
    if 0x4E00 <= cp <= 0x9FFF:
        return _jis_level1(cp)
    return cp in (0x3005, 0x303B, 0xFF01, 0xFF1F)

def _renderable(cp):
    return _fits8(cp) or _fits16(cp)

# script of a codepoint, for telling someone WHICH language will not display
_SCRIPTS = [
    (0x0600, 0x06FF, 'Arabic'),      (0x0700, 0x074F, 'Syriac'),
    (0x0900, 0x097F, 'Devanagari'),  (0x0980, 0x09FF, 'Bengali'),
    (0x0B80, 0x0BFF, 'Tamil'),       (0x0E00, 0x0E7F, 'Thai'),
    (0x0E80, 0x0EFF, 'Lao'),         (0x1000, 0x109F, 'Burmese'),
    (0x10A0, 0x10FF, 'Georgian'),    (0x0530, 0x058F, 'Armenian'),
    (0x1200, 0x137F, 'Ethiopic'),    (0xAC00, 0xD7A3, 'rare Hangul'),
    (0x4E00, 0x9FFF, 'Chinese, or rare kanji'),
]

def script_of(cp):
    for lo, hi, name in _SCRIPTS:
        if lo <= cp <= hi:
            return name
    return 'unsupported characters'

SUPPORTED = [       # (script, cell, languages) -- also the table in the README
    ('Latin + Extended-A', '8x8', 'English, Spanish, French, German, Italian, Portuguese, Dutch, '
                                  'Polish, Czech, Slovak, Hungarian, Romanian, Croatian, Turkish, '
                                  'Nordic languages, Vietnamese (unaccented)'),
    ('Greek', '8x8', 'Greek'),
    ('Cyrillic', '8x8', 'Russian, Ukrainian, Bulgarian, Serbian, Macedonian'),
    ('Hebrew', '8x8', 'Hebrew (no right-to-left layout)'),
    ('Kana + JIS level 1 kanji', '16x16', 'Japanese'),
    ('Hangul (KS X 1001)', '16x16', 'Korean'),
]

_INVISIBLE = dict.fromkeys([0x200B, 0x200C, 0x200D, 0x200E, 0x200F, 0xFEFF], None)

def check_srt_text(path):
    """(unsupported_fraction, sample, script) for a subtitle file, judged by the player's fonts."""
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            body = f.read()
    except OSError:
        return 0.0, ''
    body = re.sub(r'\d\d:\d\d:\d\d[,.]\d\d\d --> .*|^\d+$', '', body, flags=re.M)
    chars = [c for c in body if not c.isspace()]
    bad = [c for c in chars if not _renderable(ord(c))]
    sample = ''.join(sorted(set(bad))[:10])
    script = script_of(ord(bad[len(bad) // 2])) if bad else ''
    return (len(bad) / len(chars) if chars else 0.0), sample, script

def srt_lang(path):
    """Best guess at a subtitle file's language. Handles 'Movie.eng.srt', 'Movie-en.srt',
    'Movie_es-419.srt' and 'Movie.English.srt'. Returns None when there is nothing to go on.

    The trailing code wins over a language word in the name: packs often carry the source
    language in every filename ("Movie.English.srt", "Movie.English-fr.srt") with only the
    suffix telling them apart."""
    base = os.path.splitext(os.path.basename(path))[0].lower()
    toks = [t for t in re.split(r'[._\-\[\] ]+', base) if t]
    last = toks[-1] if toks else ''
    # "…-es-419", "…-pt-br": a trailing region belongs to the code before it
    if len(toks) >= 2:
        pair = f'{toks[-2]}-{last}'
        if pair in LANG3:
            return LANG3[pair]
        if re.fullmatch(r'\d{3}', last):
            last = toks[-2]
    if re.fullmatch(r'[a-z]{2,3}', last):
        return to3(last)
    for name, code in LANGNAMES.items():
        if re.search(rf'(^|[^a-z]){name}([^a-z]|$)', base):
            return code
    return None

def ffprobe(path):
    rc, out, err = _run([FFPROBE, '-v', 'error', '-show_streams', '-of', 'json', path], capture=True)
    if rc != 0 or not out.strip():
        raise RuntimeError(f'ffprobe could not read {os.path.basename(path)}: {err.strip()[:200]}')
    return json.loads(out)['streams']

def parse_name(fn):
    b = re.sub(r'\.(mkv|mp4|moflex|m4v|mov)$', '', os.path.basename(fn), flags=re.I)
    ep = re.search(r'[. _]S(\d+)E(\d+)', b, re.I)
    se = (int(ep.group(1)), int(ep.group(2))) if ep else None
    head = b[:ep.start()] if ep else b
    ym = re.search(r'[(. _](19\d\d|20\d\d)[). _]', head)
    year = int(ym.group(1)) if ym else 0
    if ym:
        head = head[:ym.start()]
    return re.sub(r'[._]+', ' ', head).strip(' -'), year, se

# ---- TMDB (optional; the user supplies their own read token) -------------------------------

def tmdb(token, path, **params):
    q = '&'.join(f'{k}={urllib.request.quote(str(v))}' for k, v in params.items())
    req = urllib.request.Request(f'https://api.themoviedb.org/3/{path}' + (f'?{q}' if q else ''))
    req.add_header('Authorization', f'Bearer {token}')
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.load(r)

def fetch_meta(token, title, year, se, log):
    """Return an info dict from TMDB (best-effort; returns {} on any failure)."""
    try:
        info = {}
        if se:
            hit = tmdb(token, 'search/tv', query=title,
                       **({'first_air_date_year': year} if year else {}))['results'][0]
            det = tmdb(token, f"tv/{hit['id']}")
            info = dict(title=det['name'], year=int((det.get('first_air_date') or '0')[:4] or year or 0),
                        genres=[g['name'] for g in det['genres']], showdesc=det.get('overview', ''),
                        runtime=(det.get('episode_run_time') or [0])[0], poster=det.get('poster_path'))
            ep = tmdb(token, f"tv/{hit['id']}/season/{se[0]}/episode/{se[1]}")
            info['eptitle'] = ep.get('name', '')
            info['date'] = ep.get('air_date', '')
            info['desc'] = (f'S{se[0]}.E{se[1]} "{ep["name"]}" - {ep.get("overview","")}'
                            if ep.get('name') else info['showdesc'])
            if ep.get('runtime'):
                info['runtime'] = ep['runtime']
        else:
            hit = tmdb(token, 'search/movie', query=title, **({'year': year} if year else {}))['results'][0]
            det = tmdb(token, f"movie/{hit['id']}")
            info = dict(title=det['title'], year=int((det.get('release_date') or '0')[:4] or year or 0),
                        genres=[g['name'] for g in det['genres']], desc=det.get('overview', ''),
                        runtime=det.get('runtime') or 0, poster=det.get('poster_path'))
        log(f"TMDB: {info['title']} ({info.get('year')})  {', '.join(info.get('genres', []))}")
        return info
    except Exception as ex:
        log(f"TMDB lookup failed ({ex}) — continuing without it")
        return {}

def download_poster(tmdb_path, dest, size='w500'):
    """Fetch a TMDB poster path ("/abc.jpg") to a local file and return that path."""
    urllib.request.urlretrieve(f'https://image.tmdb.org/t/p/{size}{tmdb_path}', dest)
    return dest

IMAGE_SUBS = {'hdmv_pgs_subtitle', 'dvd_subtitle', 'dvdsub', 'xsub', 'pgssub'}

def _duration(video):
    """Container duration in seconds, or None if ffprobe won't say."""
    rc, out, _ = _run([FFPROBE, '-v', 'error', '-show_entries', 'format=duration',
                       '-of', 'default=nk=1:nw=1', video], capture=True)
    try:
        return float(out.strip()) if rc == 0 else None
    except ValueError:
        return None

_TS = re.compile(r'(\d\d):(\d\d):(\d\d)[,.](\d\d\d)')

def shift_srt(src, dst, offset):
    """Copy an .srt with every cue moved by `offset` seconds (cues that would land before
    zero are clamped to it)."""
    off_ms = int(round(offset * 1000))

    def bump(m):
        ms = (int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))) * 1000 \
             + int(m.group(4))
        ms = max(0, ms + off_ms)
        return f'{ms//3600000:02d}:{ms//60000%60:02d}:{ms//1000%60:02d},{ms%1000:03d}'

    with open(src, encoding='utf-8', errors='replace') as f:
        body = f.read()
    with open(dst, 'w', encoding='utf-8') as f:
        f.write(_TS.sub(bump, body))
    return dst

def probe_source(video):
    """Summarize a source file for the GUI: audio tracks, text subs, image subs.
    Returns {'audio': [(index, label)], 'subs': [(index, label)], 'imagesubs': n}."""
    streams = ffprobe(video)
    out = {'audio': [], 'subs': [], 'imagesubs': 0}
    for s in streams:
        tags = s.get('tags', {}) or {}
        lang = to3(tags.get('language', 'und'))
        if s['codec_type'] == 'audio':
            desc = f"{lang}  {s.get('codec_name','?')} {s.get('channels','?')}ch"
            if tags.get('title'):
                desc += f"  ({tags['title'][:40]})"
            out['audio'].append((s['index'], desc))
        elif s['codec_type'] == 'subtitle':
            if s.get('codec_name') in IMAGE_SUBS:
                out['imagesubs'] += 1
            else:
                out['subs'].append((s['index'], f"{lang}  {s.get('codec_name','?')}"))
    return out

def build(video, moflex, out_path=None, out_dir=None, extra_srts=None, boost=True,
          is3d=None, fmt='converted', tmdb_token=None, title=None, year=None, poster=None,
          audio_in=None, audio_alt=None, lang_in=None, lang_alt=None, skip_audio=False,
          use_source_subs=True,
          offset=0.0, srt_offset=0.0, meta=None, se=None, log=print, cancel=None):
    """Run the whole pipeline. Returns the output path. `log` is a callback for progress text,
    `cancel` an optional threading.Event that aborts at the next checkpoint."""
    extra_srts = extra_srts or []

    def ck():
        if cancel is not None and cancel.is_set():
            raise Cancelled()

    if not tools_ready():
        raise RuntimeError('ffmpeg / ffprobe not found — install ffmpeg or point the app at it.')
    # scratch next to the output when we can: temp dirs are often on a small system volume and
    # the WAVs run ~600 MB per hour of audio.
    scratch_root = os.path.abspath(out_dir or os.path.dirname(os.path.abspath(out_path or moflex)))
    os.makedirs(scratch_root, exist_ok=True)     # the CLI can be pointed at a folder that is
                                                 # not there yet; the core would fail at the
                                                 # final write, after all the work was done
    work = tempfile.mkdtemp(prefix='smoflex_',
                            dir=scratch_root if os.path.isdir(scratch_root) else None)
    try:
        t_parsed, y_parsed, se_auto = parse_name(video or moflex)   # no source -> use the encode
        # se: None = decide from the filename, False = definitely not an episode (a short or a
        # music video whose name happens to look like one), (season, episode) = say so outright
        se = se_auto if se is None else (None if se is False else se)
        title = title or t_parsed
        year = year if year else y_parsed
        if is3d is None:
            enl = os.path.basename(moflex).lower()
            is3d = any(k in enl for k in ('(3d)', 'sbs', 'lrf', 'over-under', 'hsbs'))
        log(f"Title: {title!r}  Year: {year or '?'}  {'3D' if is3d else '2D'}  "
            + (f'S{se[0]:02d}E{se[1]:02d}' if se else '(movie)'))

        streams = ffprobe(video) if video else []
        audio = [s for s in streams if s['codec_type'] == 'audio']
        subs = [s for s in streams if s['codec_type'] == 'subtitle']
        if skip_audio:
            # the encode's own audio is already right: touch nothing, and skip the extraction
            # and ADPCM encode entirely -- this is what makes a subtitles-only pass quick
            a_eng = a_alt = eng_wav = alt_wav = None
            in_lang = alt_lang = None
            log('Keeping the encode\'s own audio — no extraction, no re-encode.')
        if not audio and not skip_audio:
            raise RuntimeError('source has no audio')
        byidx = {s['index']: s for s in audio}
        if not skip_audio:
            # in-band = the track stock players get (English by default); alt rides in the trailer
            a_eng = (byidx.get(audio_in) if audio_in is not None else None) or \
                    next((s for s in audio if (s.get('tags', {}) or {}).get('language') == 'eng'),
                         audio[0])
            if audio_alt is not None:
                a_alt = byidx.get(audio_alt) if audio_alt >= 0 else None   # -1 = none
            else:
                a_alt = next((s for s in audio if s is not a_eng), None)
            if a_alt is a_eng:
                a_alt = None
            # A rip with no language tags probes as UND, and UND is what the player would show on
            # the audio button. Caller-supplied labels win; otherwise fall back to the tag, and to
            # ENG for the in-band track (the one every player hears).
            det_in = to3((a_eng.get('tags', {}) or {}).get('language', 'und'))
            det_alt = to3((a_alt.get('tags', {}) or {}).get('language', 'und')) if a_alt else None
            in_lang = (lang_in or (det_in if det_in != 'UND' else 'ENG')).upper()[:3]
            alt_lang = ((lang_alt or det_alt or 'UND').upper()[:3]) if a_alt else None

        # An encode often starts from a different master than the source file: a distributor
        # logo the rip lacks, a trimmed head, and so on. `offset` slides everything taken from
        # the source (both audio tracks and every subtitle) onto the encode's timeline.
        # The delay is a FILTER, so it has to join the same -af chain as the loudness stage:
        # ffmpeg keeps only the last -af on a command, so passing it separately silently
        # replaced the whole loudness filter and every build with a positive offset came out
        # un-normalised.
        pre, post = [], []
        delay_f = f'adelay={int(round(offset * 1000))}:all=1' if offset > 0 else None
        if offset < 0:
            pre = ['-ss', f'{-offset:.3f}']
        dur = _duration(video) if video else None
        if offset and dur:          # keep the length the source had: silence in at the head,
            post += ['-t', f'{dur:.3f}']   # the same amount pushed off the tail

        def wav(stream, tag):
            """Extract one audio track, LOUDNESS-normalised when `boost` is on.

            Peak normalisation (what --normalize does downstream) cannot make a film audible on a
            handheld: cinema mixes already peak near full scale, so scaling the loudest SAMPLE to
            -1 dBFS changes nothing -- measured on Totoro, both the original encode and a
            peak-normalised rebuild sat near -28 LUFS while the peak was already -2.9 dBFS.
            Perceived loudness is the AVERAGE level, so target that instead: EBU R128 to -16 LUFS
            (the streaming standard) with a true-peak ceiling of -1 dBTP, and LRA=11 to pull the
            quiet dialogue up toward the loud effects rather than just moving the whole thing.
            The limiter is what makes "as loud as possible without clipping" actually achievable.
            Two passes: measure, then correct with those measurements.

            The fold-down to stereo has to happen BEFORE the loudness stage, inside the filter
            chain. `-ac 2 -ar 44100` are output options, applied after -af: loudnorm would
            normalise the 5.1 mix to -1 dBTP and the downmix that followed would then attenuate
            it to keep the fold-down from clipping, throwing away most of the gain. Measured on
            a 5.1 source: -13.82 LUFS the right way round, -21.74 LUFS the wrong way.
            """
            p = os.path.join(work, f'a.{tag}.wav')
            # normalise exactly the signal that gets written: stereo, 44.1 kHz
            head = 'aformat=sample_rates=44100:channel_layouts=stereo'
            ln = 'I=-16:TP=-1.0:LRA=11'
            chain = [head]
            if boost:
                # loudnorm prints its measurements at INFO level: with -v error the JSON never
                # appears, the parse fails, and this silently falls back to peak normalising --
                # i.e. to exactly the behaviour being replaced. Measure pass runs at -v info.
                meas_cmd = [FFMPEG, '-y', '-v', 'info'] + pre + ['-i', video,
                            '-map', f"0:{stream['index']}",
                            '-af', f'{head},loudnorm={ln}:print_format=json',
                            '-f', 'null', '-']
                rc, _, meas = _run(meas_cmd, cancel, capture=True)
                try:
                    j = json.loads(meas[meas.rindex('{'):meas.rindex('}') + 1])
                    # ONE CONSTANT GAIN -- never the loudnorm filter itself.
                    #
                    # loudnorm with linear=true silently falls back to DYNAMIC COMPRESSION
                    # whenever the gain needed to reach the target would breach the true-peak
                    # ceiling, which is most film masters. It squashed The Wind Rises from
                    # LRA 17.5 to 10.8 -- gain wandering 1.97x to 4.33x inside 90 seconds --
                    # and that is audible as crackle once it goes through the ADPCM encoder.
                    # Changing the volume must not change the dynamics.
                    #
                    # Take the smaller of "what reaches -16" and "what the peak allows". A film
                    # that cannot reach the target without squashing just ends up quieter.
                    #
                    # And a FLOOR on the reduction. `room` goes NEGATIVE on a hot master --
                    # plenty of them measure +1 to +5 dBTP -- and the ceiling then quietly turns
                    # the film DOWN by several decibels, which is an audible loss on a handheld.
                    # The only reason to keep headroom is that IMA-ADPCM decode can overshoot a
                    # full-scale sample, and that is worth a few tenths, not several dB. The
                    # -1.0 dBTP figure came from loudnorm's TARGET, where it was a LIMITER
                    # ceiling; it does not belong as a licence to attenuate.
                    meas_i, meas_tp = float(j['input_i']), float(j['input_tp'])
                    want = -16.0 - meas_i
                    room = -1.0 - meas_tp
                    # Floor the PEAK constraint, not the result: clamping the combined value
                    # would also block a legitimate loudness REDUCTION on a master that is
                    # already far louder than the target.
                    gain = min(want, max(room, MAX_CUT))
                    chain.append(f'volume={gain:.2f}dB')
                    if abs(gain - MAX_CUT) < 0.01 and room < MAX_CUT <= want:
                        note = (f' (hot master {meas_tp:+.2f} dBTP; reduction held at '
                                f'{MAX_CUT:.1f} dB instead of {room:+.2f})')
                    elif gain < want - 0.01:
                        note = f' (capped from {want:+.2f} dB by true peak)'
                    else:
                        note = ''
                    log(f'  {tag}: {meas_i:.2f} LUFS (range {j["input_lra"]} LU) '
                        f'{gain:+.2f} dB -> {meas_i + gain:.2f} LUFS{note}')
                except Exception:
                    # Say this loudly. A silent fall-back to peak normalising is how a whole
                    # library got built too quiet to hear on the handheld in the first place.
                    log(f'  WARNING: {tag}: loudness measurement failed — this track falls back '
                        f'to PEAK normalising and will probably be too quiet. Check the file.')
            if delay_f:                       # time shift last: kept out of the measurement
                chain.append(delay_f)
            # loudnorm upsamples to 192 kHz internally for true-peak detection and EMITS at
            # 192 kHz, so the rate has to be put back explicitly -- the WAV's rate is what the
            # core writes into the moflex audio descriptor. Resampling AFTER the loudness stage
            # is safe (the -1 dBTP ceiling leaves room); downmixing after it is not, which is
            # why the fold-down stays at the head of the chain.
            chain.append('aresample=44100')
            cmd = [FFMPEG, '-y', '-v', 'error'] + pre + ['-i', video,
                   '-map', f"0:{stream['index']}", '-af', ','.join(chain)]
            rc, _, err = _run(cmd + post + ['-c:a', 'pcm_s16le', p], cancel)
            if rc != 0 or not os.path.exists(p) or os.path.getsize(p) < 1024:
                raise RuntimeError(f'audio extraction failed: {err.strip()[:200]}')
            with wave.open(p, 'rb') as h:     # this rate becomes the moflex's audio rate
                if (h.getframerate(), h.getnchannels(), h.getsampwidth()) != (44100, 2, 2):
                    raise RuntimeError(f'extracted {h.getframerate()} Hz x{h.getnchannels()} '
                                       f'{h.getsampwidth()*8}-bit — expected 44100 x2 16-bit')
            return p
        if not skip_audio:
            log(f"Extracting audio (in-band {in_lang}"
                + (f" + trailer {alt_lang}" if a_alt else '') + ') — this is the slow part…')
            eng_wav = wav(a_eng, 'eng')
            ck()
            alt_wav = wav(a_alt, 'alt') if a_alt else None
            ck()

        # subtitles: text tracks from the container, then any user-supplied SRTs
        srt_files = []; seen = set(); skipped = 0
        if subs and not use_source_subs:
            log(f"Source has {len(subs)} subtitle track(s) — not used (the box is unticked).")
        for st in (subs if use_source_subs else []):
            if st.get('codec_name') in IMAGE_SUBS:
                skipped += 1; continue
            c = to3((st.get('tags', {}) or {}).get('language', 'und'))
            p = os.path.join(work, f's{st["index"]}.{c}.srt')
            rc, _, err = _run([FFMPEG, '-y', '-v', 'error', '-i', video,
                               '-map', f"0:{st['index']}", p], cancel)
            if rc == 0 and os.path.exists(p) and os.path.getsize(p) > 0:
                # two tracks can share a language (a plain and an SDH English): keep both
                srt_files.append((c, p, True)); seen.add(c)       # True = came from the source
                log(f"  source subtitle track {st['index']}: {c}, {os.path.getsize(p)} B")
            else:
                log(f"  source subtitle track {st['index']} ({c}) could not be extracted"
                    + (f": {err.strip()[:120]}" if err.strip() else ' (empty)'))
        if skipped:
            log(f"NOTE: {skipped} of the source's subtitle tracks are image-based (PGS/VobSub) and "
                f"cannot be used — they are pictures, not text. Add text .srt files instead.")
        if use_source_subs and not subs:
            log('Source has no subtitle tracks of its own.'
                if video else 'No source video, so no subtitles could come from one.')
        # ENG is the default track (first wins), JPN next, everything else alphabetical
        _rank = {'ENG': 0, 'JPN': 1}
        srt_files.sort(key=lambda x: (_rank.get(x[0], 9), x[0]))
        explicit = []
        for item in extra_srts:
            # an entry may be a plain path (guess the language) or (code, path) when the caller
            # already knows better than any filename heuristic
            if isinstance(item, (tuple, list)):
                c, p = (item[0] or '').strip().upper()[:3] or 'SUB', item[1]
            else:
                p = item
                c = srt_lang(p) or 'SUB'
            if c in seen:            # never silently drop a file the user added by hand
                log(f'NOTE: {os.path.basename(p)} is another {c} track — both are kept.')
            explicit.append((c, p, False)); seen.add(c)          # False = supplied by hand
        # A caller that spells out its own codes has also chosen the ORDER, and the first track
        # is the default one, so that list is kept exactly as given and goes first.
        if any(isinstance(i, (tuple, list)) for i in extra_srts):
            srt_files = explicit + srt_files
        else:
            srt_files = sorted(srt_files + explicit, key=lambda x: (_rank.get(x[0], 9), x[0]))
        # Subtitles pulled OUT of the source share its timeline, so they move with the audio.
        # Files supplied by hand were timed to whatever release they were made for -- often the
        # same master the encode came from -- so they get their own knob and default to
        # untouched. (Downloaded subs for a film whose encode has a longer head are a real case:
        # the audio needed +14.4 s while the subtitles were already correct.)
        # The core reads each subtitle's language off its FILENAME, so every file is copied
        # (or shifted) into the scratch dir as "s.<CODE>.srt" first. Passing user paths straight
        # through meant anything not already named that way was tagged SUB -- and several such
        # files collapsed onto one label.
        moved = []
        for i, (c, p, from_source) in enumerate(srt_files):
            sh = offset if from_source else srt_offset
            # the index keeps two tracks with the same code from overwriting one another; the
            # core only reads the ".<CODE>.srt" tail, so a prefix is invisible to it
            dst = os.path.join(work, f'x{i}.{c}.srt')
            if sh:
                shift_srt(p, dst, sh)
            elif os.path.abspath(p) != os.path.abspath(dst):
                shutil.copyfile(p, dst)
            # zero-width marks and BOMs have no glyph and would show as '?'
            with open(dst, encoding='utf-8', errors='replace') as f:
                body = f.read()
            cleaned = body.translate(_INVISIBLE)
            if cleaned != body:
                with open(dst, 'w', encoding='utf-8') as f:
                    f.write(cleaned)
            frac, sample, script = check_srt_text(dst)
            if frac > 0.2:
                log(f'WARNING: the {c} track is {script} — the player has no glyphs for it, so '
                    f'every line would show as "?" ({frac*100:.0f}% of characters, e.g. {sample}). '
                    f'Leave this track out.')
            elif frac > 0.01:
                log(f'NOTE: {c} has {frac*100:.1f}% characters with no glyph ({sample}) — mostly '
                    f'they will show as "?".')
            moved.append((c, dst))
        srt_files = moved
        if offset or srt_offset:
            log(f'Offsets applied: audio {offset:+.3f} s, extracted subtitles {offset:+.3f} s, '
                f'supplied subtitles {srt_offset:+.3f} s.')
        log(f"Subtitles: {', '.join(c for c, _ in srt_files) or 'none'}")

        # metadata: defaults <- TMDB (if a token is set) <- whatever the caller filled in by hand
        info = {'title': title, 'year': year, 'genres': [], 'runtime': 0, 'date': '',
                'desc': '', 'showdesc': '', 'poster': poster}
        if tmdb_token:
            fetched = fetch_meta(tmdb_token, title, year, se, log)
            # TMDB reports the poster as a bare path ("/abc.jpg"), which is not a file: keep it
            # out of `info` or it would overwrite a poster the caller handed us with something
            # that cannot be opened
            tmdb_poster = fetched.pop('poster', None)
            info.update({k: v for k, v in fetched.items() if v})
            if tmdb_poster and not poster:
                try:
                    info['poster'] = download_poster(tmdb_poster, os.path.join(work, 'poster.jpg'))
                    log('Poster downloaded from TMDB.')
                except Exception as ex:
                    log(f'Poster download failed ({ex}) — building without artwork.')
        if meta:                       # hand-entered values always win over anything fetched
            info.update({k: v for k, v in meta.items() if v not in (None, '', [], 0)})
            log('Using the metadata you supplied'
                + (' (over TMDB)' if tmdb_token else '') + '.')
        if info.get('poster') and not os.path.exists(str(info['poster'])):
            log(f"Poster {info['poster']} not found — building without artwork.")
            info['poster'] = None

        gen = info.get('genres') or []
        if isinstance(gen, str):                      # accept "Fantasy, Animation" as typed
            gen = [g.strip() for g in gen.split(',') if g.strip()]
        nfo = os.path.join(work, 'info.nfo')
        with open(nfo, 'w') as f:
            f.write(f"title={info['title']}\nyear={info['year']}\n"
                    f"category={info.get('category') or ('TV Shows' if se else 'Movies')}\n"
                    f"genres={', '.join(gen)}\nruntime={info.get('runtime', 0)}\n"
                    f"date={info.get('date','')}\nis3d={1 if is3d else 0}\n"
                    f"format={fmt if is3d else ''}\ndesc={info.get('desc','')}\n")
            if info.get('showdesc'):
                f.write(f"showdesc={info['showdesc']}\n")

        # output name
        if not out_path:
            tag3d = ' (3D)' if is3d else ''
            if se:
                et = info.get('eptitle', '')
                prefix = f"{info['title']} ({info['year']}){tag3d} (S) - S{se[0]:02d}e{se[1]:02d}"
                if et:
                    room = 130 - len(prefix) - 10
                    if len(et) > room > 0:
                        et = et[:room].rsplit(' ', 1)[0].rstrip(' -!?,') + '…'
                base = prefix + (f" - {et}" if et else '') + '.moflex'
            else:
                base = f"{info['title']} ({info['year']}){tag3d} (S).moflex"
            for a, b in (('/', ' - '), (':', ' -'), ('*', ''), ('?', ''), ('"', "'"),
                         ('<', ''), ('>', ''), ('|', '')):
                base = base.replace(a, b)
            base = re.sub(r'\s+', ' ', base).strip()
            out_path = os.path.join(out_dir or os.path.dirname(os.path.abspath(moflex)), base)

        # assemble via the core builder
        args = [moflex, out_path, '--nfo', nfo]
        if not alt_wav:
            # rebuilding the trailer of a file that already has one: keep whatever second
            # audio track it carries, since none is being supplied this time
            args.append('--keep-trailer-audio')
        if eng_wav:                         # skip_audio leaves the encode's own track alone
            args += ['--strip-audio', '--audio', eng_wav, '--lang', in_lang]
            # NOT --normalize. wav() has already brought this to -16 LUFS, and peak
            # normalisation on top would undo it: loudnorm caps true peak at -1 dBTP but
            # usually lands below it, so scaling the loudest sample back up to -1 dBFS adds
            # gain the loudness stage deliberately did not. Harmless on a cinema mix that hits
            # the limiter, wrong on anything quieter -- which is the material that needed the
            # loudness stage in the first place.
        if alt_wav:
            args += ['--trailer-audio', alt_wav, '--trailer-lang', alt_lang]
        for c, p in srt_files:
            args += ['--trailer-srt', p]
        if info.get('poster') and os.path.exists(info['poster']):
            args += ['--art', info['poster']]
        ck()
        log('Assembling SUPER MOFLEX (encoding audio + rewriting the container)…')
        # the repack is the long pole on a feature film, so let Cancel reach inside it
        core.CANCEL = (lambda: cancel.is_set()) if cancel is not None else None
        try:
            if log is print:                       # console already: no redirect, no loop risk
                core.main_args(args)
            else:
                with contextlib.redirect_stdout(_LogStream(log)):
                    core.main_args(args)           # in-process call (no subprocess)
        except core.Aborted:
            raise Cancelled()
        finally:
            core.CANCEL = None

        # verify the trailer wrote cleanly (an SMB/NAS write once zeroed a trailer tail, so the
        # output is always walked back after writing rather than trusted)
        secs = verify(out_path)
        log('Verified trailer: ' + ', '.join(f'{k}x{v}' if v > 1 else k for k, v in secs.items()))
        # drop the full-size poster next to the output (catalog artwork convention)
        if info.get('poster') and os.path.exists(info['poster']):
            shutil.copyfile(info['poster'], re.sub(r'\.moflex$', '.jpg', out_path, flags=re.I))
        log(f"DONE → {out_path}")
        return out_path
    finally:
        shutil.rmtree(work, ignore_errors=True)

def verify(path):
    """Walk the CSXTRA trailer of a finished file. Returns {section: count}; raises on damage."""
    known = {b'NFO0', b'ART5', b'LNG0', b'SUB0', b'SUB1', b'AUD0', b'AUD1'}
    secs = {}
    with open(path, 'rb') as f:
        f.seek(0, 2); fsz = f.tell()
        if fsz < 32:
            raise RuntimeError('verify: output is too small to be a moflex')
        f.seek(-16, 2); tail = f.read(16)
        if tail[8:] != b'CSXTRA01':
            raise RuntimeError('verify: footer missing after write')
        poff = struct.unpack('<Q', tail[:8])[0]
        if poff >= fsz - 16:
            raise RuntimeError('verify: payload offset points past the file')
        p = poff
        while p + 8 <= fsz - 16:
            f.seek(p); h = f.read(8); cc = h[:4]; ln = struct.unpack('<I', h[4:])[0]
            if cc not in known or ln == 0 or p + 8 + ln > fsz - 16:
                raise RuntimeError(f'verify: corrupt trailer section at byte {p}')
            secs[cc.decode()] = secs.get(cc.decode(), 0) + 1
            p += 8 + ln
        if p != fsz - 16:
            raise RuntimeError('verify: trailer walk did not reach the footer')
    return secs

# ---- command line (the GUI is smoflex_gui.py; this is the same pipeline headless) ------------

def _cli(argv):
    import argparse
    ap = argparse.ArgumentParser(prog='smoflex_build',
                                 description='Build a SUPER MOFLEX from a source video + an encoded .moflex')
    ap.add_argument('video', help='source MKV/MP4 (dual audio, ideally text subtitles)')
    ap.add_argument('moflex', help='the mobiclip-encoded .moflex of the same title')
    ap.add_argument('-o', '--out', help='output file path')
    ap.add_argument('-d', '--outdir', help='output folder (name is built from the metadata)')
    ap.add_argument('--srt', action='append', default=[], metavar='FILE',
                    help='extra subtitle file, repeatable; language read from "name.eng.srt"')
    ap.add_argument('--no-normalize', action='store_true', help='keep the source levels')
    ap.add_argument('--skip-audio', action='store_true',
                    help="keep the audio already in the .moflex and only add subtitles "
                         "and metadata (fast; the source video becomes optional)")
    ap.add_argument('--2d', dest='two_d', action='store_true', help='force 2D (default: from the name)')
    ap.add_argument('--3d', dest='three_d', action='store_true', help='force 3D')
    ap.add_argument('--format', choices=['native', 'converted'], default='converted',
                    help='3D origin recorded in the metadata')
    ap.add_argument('--title'), ap.add_argument('--year', type=int)
    ap.add_argument('--offset', type=float, default=0.0, metavar='SECS',
                    help='slide the source audio (and the source\'s own subtitle tracks) onto '
                         'the encode\'s timeline: positive if the encode starts earlier (extra '
                         'logo/leader the source lacks), negative if it starts later. Only '
                         'needed when the encode did not come from this exact file.')
    ap.add_argument('--srt-offset', type=float, default=0.0, metavar='SECS',
                    help='same, for --srt files only; they are usually already timed to the '
                         'encode\'s master, so this defaults to 0')
    ap.add_argument('--poster', help='poster image to embed (default: TMDB, if a token is set)')
    ap.add_argument('--lang', help='3-letter code for the in-band track (default ENG, or the '
                                   'source tag when it has one)')
    ap.add_argument('--alt-lang', help='3-letter code for the trailer track')
    ap.add_argument('--genres', help='comma separated, e.g. "Fantasy, Animation"')
    ap.add_argument('--runtime', type=int, help='minutes')
    ap.add_argument('--category', help='library category (default: Movies, or TV Shows for SxxEyy)')
    ap.add_argument('--date', help='release / air date, YYYY-MM-DD')
    ap.add_argument('--eptitle', help='episode title (used in the output filename)')
    ap.add_argument('--desc', help='description text')
    ap.add_argument('--tmdb', default=os.environ.get('TMDB_TOKEN'),
                    help='TMDB read token for metadata (or set TMDB_TOKEN)')
    ap.add_argument('--ffmpeg', help='path to ffmpeg if it is not on PATH')
    a = ap.parse_args(argv)
    if a.ffmpeg:
        set_tools(a.ffmpeg)
    is3d = True if a.three_d else (False if a.two_d else None)
    try:
        meta = {'title': a.title, 'year': a.year, 'genres': a.genres, 'runtime': a.runtime,
                'category': a.category, 'date': a.date, 'eptitle': a.eptitle, 'desc': a.desc}
        build(a.video, a.moflex, out_path=a.out, out_dir=a.outdir, extra_srts=a.srt,
              boost=not a.no_normalize, is3d=is3d, fmt=a.format, tmdb_token=a.tmdb,
              title=a.title, year=a.year, poster=a.poster, offset=a.offset,
              srt_offset=a.srt_offset, meta=meta, skip_audio=a.skip_audio,
              lang_in=a.lang, lang_alt=a.alt_lang)
    except (RuntimeError, Cancelled) as ex:
        print(f'error: {ex}', file=sys.stderr)
        return 1
    return 0

if __name__ == '__main__':
    sys.exit(_cli(sys.argv[1:]))
