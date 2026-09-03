#!/usr/bin/env python3
"""smoflex_srtclean — strip SDH / caption annotations from an .srt, leaving the dialogue.

Subtitle packs for the hearing impaired carry sound cues ("[Bee Buzzing]"), speaker labels
("MAN:") and site adverts alongside the dialogue. On a 400x240 screen those eat the line that
the actual line of dialogue needed.

  smoflex_srtclean.py in.srt [-o out.srt] [--keep-lyrics] [--dry-run]

Cues that end up empty are dropped and the rest renumbered. Timings are never touched.
"""
import argparse
import re
import sys

TS = re.compile(r'(\d\d:\d\d:\d\d[,.]\d\d\d)\s*-->\s*(\d\d:\d\d:\d\d[,.]\d\d\d)(.*)')
BRACKET = re.compile(r'\[[^\]]*\]')            # [Door Creaks]
PAREN = re.compile(r'\([^)]*\)')               # (sighs)
# "MAN:", "WOMAN #2:", "DR. SMITH:" -- an all-caps label at the start of a line
SPEAKER = re.compile(r'^\s*-?\s*[A-Z][A-Z0-9 .#\'&/-]{1,24}:\s*')
ADVERT = re.compile(r'(subtitle|subs|sync|correct|encode|rip)[^\n]*(https?://|www\.|\.com|\.org|@)',
                    re.I)
TAGS = re.compile(r'</?[a-zA-Z][^>]*>')        # <i> </i> <font ...>


def parse(text):
    cues, i, lines = [], 0, text.replace('\r\n', '\n').replace('\r', '\n').split('\n')
    while i < len(lines):
        m = TS.match(lines[i])
        if not m:
            i += 1
            continue
        start, end = m.group(1), m.group(2)
        i += 1
        body = []
        while i < len(lines) and lines[i].strip() != '':
            body.append(lines[i])
            i += 1
        cues.append([start, end, body])
    return cues


def clean_cue(body, keep_lyrics):
    out = []
    for line in body:
        s = TAGS.sub('', line)
        if ADVERT.search(s):
            continue
        if not keep_lyrics and ('\u266a' in s or '\u266b' in s):
            # a music cue with no words left is noise; lyrics keep their notes
            if not re.search(r'[A-Za-z]{3}', BRACKET.sub('', s).replace('\u266a', '')):
                continue
        s = BRACKET.sub('', s)
        s = PAREN.sub('', s)
        s = SPEAKER.sub('', s)
        s = re.sub(r'\s+', ' ', s).strip()
        s = re.sub(r'^-\s*$', '', s)                    # a dash whose speech was an annotation
        if s:
            out.append(s)
    # a dialogue dash only means something when two speakers remain: if removing an annotation
    # left just one, drop it (the wrapped second line of that speech is not a second speaker)
    if sum(1 for l in out if l.startswith('-')) == 1:
        out = [re.sub(r'^-\s*', '', l) if l.startswith('-') else l for l in out]
    return out


def clean(text, keep_lyrics=False):
    kept, dropped, edited = [], 0, 0
    for start, end, body in parse(text):
        new = clean_cue(body, keep_lyrics)
        if not new:
            dropped += 1
            continue
        if new != [re.sub(r'\s+', ' ', l).strip() for l in body]:
            edited += 1
        kept.append((start, end, new))
    out = []
    for n, (start, end, body) in enumerate(kept, 1):
        out.append(str(n))
        out.append(f'{start} --> {end}')
        out.extend(body)
        out.append('')
    return '\n'.join(out), len(kept), dropped, edited


def main():
    ap = argparse.ArgumentParser(prog='smoflex_srtclean', description=__doc__.split('\n')[0])
    ap.add_argument('src')
    ap.add_argument('-o', '--out', help='default: alongside the input, ".dialogue.srt"')
    ap.add_argument('--keep-lyrics', action='store_true', help='keep music/lyric lines')
    ap.add_argument('--dry-run', action='store_true', help='report, write nothing')
    a = ap.parse_args()
    with open(a.src, encoding='utf-8', errors='replace') as f:
        text = f.read()
    body, kept, dropped, edited = clean(text, a.keep_lyrics)
    print(f'{kept} cues kept, {dropped} dropped as annotation-only, {edited} had annotations removed')
    if a.dry_run:
        return 0
    out = a.out or re.sub(r'\.srt$', '', a.src, flags=re.I) + '.dialogue.srt'
    with open(out, 'w', encoding='utf-8') as f:
        f.write(body)
    print(f'wrote {out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
