#   MCprog - A programmer for the Motorola MC micro radio family,
#            replacing the 1987 Radio Service Software
#
#   Copyright (C) 2026  Felix Erckenbrecht, DG1YFE
#
#    This file is part of MCprog.
#
#    MCprog is free software: you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
#
#    MCprog is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License
#    along with MCprog.  If not, see <http://www.gnu.org/licenses/>.
#
#    SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the language-neutral conformance data in this directory.

Output is line-oriented, tag-first, whitespace-separated, `#` for comments -- parseable with
fgets+strtok in C and bufio.Scanner+strings.Fields in Go, with no dependency either side.
Frequencies are integer hertz and times integer milliseconds, so the two languages compare exactly
and no locale can intervene. All byte strings are lowercase hex.

Run from the repository root:  python3 testdata/gen.py
The output is committed; regenerating must produce byte-identical files.
"""
import os, re, sys, hashlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'testdata')

# --- model descriptors, spec K-20 -------------------------------------------------------------
MODELS = {
    'eva_56':  dict(size=512, cksum=0x000, chan=0x0E0, nchan=32, stride=8, tx=2, rx=5,
                    band=0x0DC, refdiv=0x0D4, numbered=True),
    'eva_sel5': dict(size=512, cksum=0x000, chan=0x0E0, nchan=32, stride=8, tx=2, rx=5,
                     band=0x0DC, refdiv=0x0D4, numbered=True),
    'eza_sel5': dict(size=256, cksum=0x000, chan=0x0C8, nchan=8, stride=6, tx=0, rx=3,
                     band=0x082, refdiv=0x0C4, numbered=False),
    # MCEZ13's checksum covers 126 of its 128 bytes, not the whole device
    'eza_cspl': dict(size=128, cksum=0x001, cklen=126, chan=0x039, nchan=8, stride=6, tx=0, rx=3,
                     band=0x037, refdiv=0x002, numbered=False),
}
# --- per-model channel flag bits, spec K-22 ----------------------------------------------------
# (name, bit, half, inverted, provenance).  half: 'both' | 'tx' | 'rx'.  These are NOT portable
# between models -- bit 3 is clock shift on the EVA and auto-acknowledge on the EZA 9.
FLAGS = {
    'eva_56': [('clock_shift', 3, 'both', 0, 'C'), ('decode', 4, 'both', 0, 'C'),
               ('tx_inhibit', 5, 'both', 0, 'C'), ('encode', 6, 'both', 0, 'C'),
               ('power_high', 7, 'both', 0, 'C')],
    'eza_sel5': [('auto_ack', 3, 'both', 0, 'C'), ('decode', 4, 'both', 0, 'C'),
                 ('tx_inhibit', 5, 'both', 0, 'C'), ('encode', 6, 'both', 0, 'C'),
                 ('clock_shift', 7, 'rx', 0, 'C'), ('power_high', 7, 'tx', 0, 'S')],
    # MCEZ13 carries almost no per-channel flags: PL lives in tables, TX inhibit is global.
    'eza_cspl': [('clock_shift', 6, 'tx', 1, 'C'), ('reserved_b7', 7, 'tx', 0, 'S')],  # offsets -2
}
FLAGS['eva_sel5'] = FLAGS['eva_56']
# --- PL / CTCSS, spec K-14 ---------------------------------------------------------------------
# The standard tone list the original software carries (EVA image, CS:0x3436): 40 little-endian
# words in tenths of a Hz, index 0 = 0.0 meaning "no PL".  Storage is round(7.984 x f_Hz)
# big-endian, the same law as the tone constants -- so 88.5 Hz stores as 707 and decodes to 88.55.
# Decoding therefore snaps to this list, which is why the original never shows 88.6.
PL_TONES = [0, 670, 693, 719, 744, 770, 797, 825, 854, 885, 915, 974, 1000, 1035, 1072, 1109,
            1148, 1188, 1230, 1273, 1318, 1365, 1413, 1462, 1514, 1567, 1622, 1679, 1738, 1799,
            1862, 1928, 2035, 2065, 2107, 2181, 2257, 2336, 2418, 2503]

# Per-model PL layout, all measured by write-back oracle.  MCEZ13 is deliberately absent: its
# tables are known but the per-channel indexing is not, and its read is still blocked.
PL = {
    'eva_56':   dict(tone=0x047, list=0x047, count=0x0CE, mode=0x1FD, max=10),
    'eva_sel5': dict(tone=0x047, list=0x047, count=0x0CE, mode=0x1FD, max=10),
    'eza_sel5': dict(tone=0x02F, list=0x031, count=0x083, mode=0x07F, max=10),
    # MCEZ13 has no mode byte: an encoder table and, uniquely, a decoder table on its own law.
    'eza_cspl': dict(tone=0x022, list=0x022, count=None, mode=None, dec=0x00E, max=10),
}


def pl_encode(dhz):
    """round(7.984 * f_Hz) with f in tenths of a Hz, in integers."""
    return (7984 * dhz + 5000) // 10000


def pl_dec_encode(dhz):
    """MCEZ13's PL DECODER law: round(61.107 x f_Hz), against the encoder's round(7.984 x f)."""
    return (61107 * dhz + 5000) // 10000


def pl_dec_decode(word):
    for d in PL_TONES:
        if d and pl_dec_encode(d) == word:
            return d
    return (word * 10000 + 30553) // 61107


def pl_decode(word):
    """Nearest standard tone, or the computed value in tenths of a Hz when none matches."""
    for d in PL_TONES:
        if d and pl_encode(d) == word:
            return d
    return (word * 10000 + 3992) // 7984


P_BY_BAND = {1: 80, 2: 80, 3: 128, 4: 254}
IF_HZ = 21_400_000


def decode(b0, b1, b2, P):
    return ((((b0 & 3) << 8) | b1) * P + b2) * (3125 if b0 & 4 else 2500)


def encode(hz, P, step):
    """Canonical encoding, spec K-11: always emits b2 < P.

    Returns (None, None) when the frequency does not fit the 10-bit coarse field.  U-3 forbids
    clamping, so callers must surface this as an error rather than wrapping it.
    """
    n, rem = divmod(hz // step, P)
    if hz % step or n > 1023:
        return None, None
    return n, rem


def vec_codeplug(path, model, band_hint=None):
    m = MODELS[model]
    e = open(os.path.join(ROOT, path), 'rb').read()
    L = ['# generated by testdata/gen.py -- do not edit',
         '# source: %s sha256:%s' % (path, hashlib.sha256(e).hexdigest()[:16]),
         'IMG    %s' % path,
         'MODEL  %s size=%d' % (model, len(e)),
         'SUM    stored=0x%02x total=0x%02x valid=%d'
         % (e[m['cksum']], sum(e[:m.get('cklen') or len(e)]) & 0xFF,
            1 if sum(e[:m.get('cklen') or len(e)]) & 0xFF == 0xFF else 0)]
    band = (e[m['band']] >> 4) & 7
    P = P_BY_BAND.get(band)
    L.append('BAND   index=%d p=%s raster=%d' % (band, P if P else 'none', (e[m['band']] >> 7) & 1))
    L.append('REFDIV %04x %04x' % (e[m['refdiv']] << 8 | e[m['refdiv'] + 1],
                                   e[m['refdiv'] + 2] << 8 | e[m['refdiv'] + 3]))
    # channel table: terminated, not sparse (K-23)
    term = None
    if m['numbered']:
        for i in range(m['nchan']):
            if e[m['chan'] + i * m['stride']] == 0xFF:
                term = i
                break
    n = term if term is not None else m['nchan']
    L.append('CHANS  terminated=%d slots=%d terminator=%s'
             % (n, m['nchan'], term + 1 if term is not None else 'none'))
    if P:
        for i in range(n):
            o = m['chan'] + i * m['stride']
            tx, rx = e[o + m['tx']:o + m['tx'] + 3], e[o + m['rx']:o + m['rx'] + 3]
            f_tx, f_rx = decode(*tx, P), decode(*rx, P) + IF_HZ
            # K-11: is the stored form canonical?
            canon = 1 if tx[2] < P and rx[2] < P else 0
            num = '%02x' % e[o] if m['numbered'] else '--'
            # K-24: allocated but unprogrammed is a distinct state from programmed, and must
            # never be rendered as 0.00000 MHz.
            state = 'empty' if (f_tx == 0 and f_rx == IF_HZ) else 'prog'
            L.append('CHAN   %-2d num=%s tx=%d rx=%d txraw=%s rxraw=%s canon=%d state=%s'
                     % (i + 1, num, f_tx, f_rx, tx.hex(), rx.hex(), canon, state))
        if term is not None:
            for i in range(term, m['nchan']):
                o = m['chan'] + i * m['stride']
                L.append('STALE  %-2d raw=%s' % (i + 1, e[o:o + m['stride']].hex()))
    else:
        L.append('NOTE   band unprogrammed -- frequencies are not computable (spec K-10)')
    pl = PL.get(model)
    if pl and pl['mode'] is None:
        # MCEZ13: both tables, always present
        L.append('PL     mode=table count=%d' % pl['max'])
        L.append('PLLIST %s' % ' '.join(
            '%.1f' % (pl_decode(w) / 10) if w else '-'
            for w in [(e[pl['list'] + 2 * i] << 8) | e[pl['list'] + 2 * i + 1]
                      for i in range(pl['max'])]))
        L.append('PLDEC  %s' % ' '.join(
            '%.1f' % (pl_dec_decode(w) / 10) if w else '-'
            for w in [(e[pl['dec'] + 2 * i] << 8) | e[pl['dec'] + 2 * i + 1]
                      for i in range(pl['max'])]))
    elif pl:
        mode = {0x60: 'single', 0xE0: 'selectable'}.get(e[pl['mode']] & 0xF0, 'off')
        if mode == 'off':
            # With PL disabled the count and list bytes hold unrelated data; rendering them as
            # tones would be the same mistake as printing an unprogrammed channel as 0.00000.
            L.append('PL     mode=off')
        elif mode == 'single':
            w = (e[pl['tone']] << 8) | e[pl['tone'] + 1]
            L.append('PL     mode=single tone=%.1f' % (pl_decode(w) / 10))
        else:
            n = min(e[pl['count']] >> 4, pl['max'])
            tones = [(e[pl['list'] + 2 * i] << 8) | e[pl['list'] + 2 * i + 1] for i in range(n)]
            L.append('PL     mode=selectable count=%d' % n)
            L.append('PLLIST %s' % ' '.join('%.1f' % (pl_decode(w) / 10) if w else '-'
                                            for w in tones))
    if model in AAK:
        # bit 7 is not part of the value; count 0 means "nothing here", not 0 ms
        count = e[AAK[model]] & 0x7F
        L.append('AAK    %s' % ('none' if count == 0 else
                                'ms=%d count=%d' % (aak_decode(count), count)))
    open(os.path.join(OUT, 'codeplug', os.path.basename(path).split('.')[0].lower() + '.vec'),
         'w').write('\n'.join(L) + '\n')
    return len(L)


def vec_freq():
    """Every channel field in every sample, both directions, plus boundary cases."""
    L = ['# frequency codec vectors, spec K-10 / K-11',
         '# DEC raw=<6 hex> p=<P> hz=<integer Hz>   canon=1 if b2 < P',
         '# ENC hz=<Hz> p=<P> step=<Hz> flags=<b0 bits 3-7 to preserve> want=<3 hex>',
         '#     encoders must emit b2 < P (K-11) and must not disturb the flag bits (K-22)']
    seen = set()
    for path, model in SAMPLES:
        m = MODELS[model]
        e = open(os.path.join(ROOT, path), 'rb').read()
        band = (e[m['band']] >> 4) & 7
        P = P_BY_BAND.get(band)
        if not P:
            continue
        for i in range(m['nchan']):
            o = m['chan'] + i * m['stride']
            if m['numbered'] and e[o] == 0xFF:
                break
            for half in (m['tx'], m['rx']):
                f = e[o + half:o + half + 3]
                if f.hex() in seen:
                    continue
                seen.add(f.hex())
                L.append('DEC raw=%s p=%d hz=%d canon=%d'
                         % (f.hex(), P, decode(*f, P), 1 if f[2] < P else 0))
    for hz, P, step, flags in [(145_000_000, 80, 3125, 0x96), (160_000_000, 80, 3125, 0x96),
                               (136_000_000, 80, 3125, 0x96), (434_600_000, 254, 3125, 0xDE)]:
        n, rem = encode(hz, P, step)
        L.append('ENC hz=%d p=%d step=%d flags=%02x want=%02x%02x%02x'
                 % (hz, P, step, flags, (flags & 0xF8) | ((n >> 8) & 3) | 4, n & 0xFF, rem))
    open(os.path.join(OUT, 'freq', 'roundtrip.vec'), 'w').write('\n'.join(L) + '\n')
    return len(L)


def _halves(m, flag):
    _, bit, half, inv, _p = flag
    return {'both': (m['tx'], m['rx']), 'tx': (m['tx'],), 'rx': (m['rx'],)}[half]


def _cksum_fix(e, m):
    e[m['cksum']] = (e[m['cksum']] - ((sum(e) & 0xFF) - 0xFF)) & 0xFF


def vec_edit():
    """K-22 / K-30: an edit must move the edited field and the checksum, and nothing else."""
    L = ['# edit vectors, spec K-11 / K-22 / K-30',
         '# EDIT img=<path> model=<m> op=<op> slot=<1-based> arg=<value>',
         '#      changed=<off>:<old>:<new>|... , offsets in hex, ALWAYS including the checksum',
         '# ops: set_tx <Hz>  set_rx <displayed Hz>  flag:<name> <0|1>']
    for path, model, ops in EDITS:
        m = MODELS[model]
        base = open(os.path.join(ROOT, path), 'rb').read()
        band = (base[m['band']] >> 4) & 7
        P = P_BY_BAND.get(band)
        step = 3125 if (base[m['band']] >> 7) & 1 else 2500
        for op, slot, arg in ops:
            e = bytearray(base)
            o = m['chan'] + (slot - 1) * m['stride']
            if op in ('set_tx', 'set_rx'):
                hz = arg - (IF_HZ if op == 'set_rx' else 0)
                half = m['tx'] if op == 'set_tx' else m['rx']
                # K-24a: an empty slot's flag bits are leftovers.  On MCEZ13 clock shift is stored
                # inverted, so a zeroed record reads as clock shift ON; programming it would
                # inherit that silently.  Default it off, leaving programmed channels alone.
                tx3 = e[o + m['tx']:o + m['tx'] + 3]
                rx3 = e[o + m['rx']:o + m['rx'] + 3]
                was_empty = decode(*tx3, P) == 0 and decode(*rx3, P) == 0
                n, rem = encode(hz, P, step)
                if n is None:
                    L.append('EDIT img=%s model=%s op=%s slot=%d arg=%d changed=unrepresentable'
                             % (path, model, op, slot, arg))
                    continue
                flags = e[o + half]
                e[o + half] = (flags & 0xF8) | ((n >> 8) & 3) | (4 if step == 3125 else 0)
                e[o + half + 1] = n & 0xFF
                e[o + half + 2] = rem
                if was_empty:
                    cs = next((f for f in FLAGS[model] if f[0] == 'clock_shift'), None)
                    if cs:
                        _, bit, _h, inv, _p = cs
                        for hh in _halves(m, cs):
                            if inv:                      # inverted: bit SET means off
                                e[o + hh] |= 1 << bit
                            else:
                                e[o + hh] &= ~(1 << bit) & 0xFF
            else:
                name = op.split(':', 1)[1]
                fl = next(f for f in FLAGS[model] if f[0] == name)
                _, bit, _h, inv, _p = fl
                on = arg ^ inv                       # inverted flags store the opposite sense
                for half in _halves(m, fl):
                    if on:
                        e[o + half] |= 1 << bit
                    else:
                        e[o + half] &= ~(1 << bit) & 0xFF
            _cksum_fix(e, m)
            chg = '|'.join('%03x:%02x:%02x' % (i, base[i], e[i])
                           for i in range(len(base)) if base[i] != e[i])
            L.append('EDIT img=%s model=%s op=%s slot=%d arg=%d changed=%s'
                     % (path, model, op, slot, arg, chg or 'none'))
    open(os.path.join(OUT, 'edit', 'edits.vec'), 'w').write('\n'.join(L) + '\n')
    return len(L)


AAK = {'eza_sel5': 0x076}   # K-15: measured on the EZA 9 only
AAK_MIN_MS, AAK_MAX_MS = 16, 1984


def aak_encode(ms):
    """round(ms / 15.625) -- a count of 1/64 s."""
    return (ms * 64 + 500) // 1000


def aak_decode(count):
    return (count * 15625 + 500) // 1000


def vec_aak():
    """The auto-acknowledge delay codec, spec K-15."""
    L = ['# Auto-acknowledge delay, spec K-15',
         '# AAKENC ms=<n> count=<n>     round(ms / 15.625), the count is 1/64 s',
         '# AAKDEC count=<n> ms=<n>',
         '# AAKBAD ms=<n>               outside 16-1984: refused, never clamped (U-3)',
         '# AAKMAP model=<m> off=<off>  0 where the field is not established for that model',
         '#',
         '# 16/32/208/500/1000/1500/1984 ms were driven through MCEZ9R and read back off the wire;',
         '# the counts are what the 1987 software actually stored.  203 is the factory default,',
         '# which is what the editor shows for the count it ships with.']
    for ms in (16, 32, 203, 208, 500, 1000, 1500, 1984):
        L.append('AAKENC ms=%d count=%d' % (ms, aak_encode(ms)))
    for c in (1, 2, 13, 32, 64, 96, 127):
        L.append('AAKDEC count=%d ms=%d' % (c, aak_decode(c)))
    for ms in (0, 1, 15, 1985, 2000, 65535):
        L.append('AAKBAD ms=%d' % ms)
    for m in sorted(MODELS):
        L.append('AAKMAP model=%s off=%03x' % (m, AAK.get(m, 0)))
    open(os.path.join(OUT, 'aak', 'aak.vec'), 'w').write('\n'.join(L) + '\n')
    return len(L)


def vec_pl():
    """The PL tone codec and the standard list, spec K-14."""
    L = ['# PL / CTCSS vectors, spec K-14',
         '# TONE idx=<n> dhz=<tenths of a Hz> word=<4 hex>   the standard list, index 0 = no PL',
         '# PLENC dhz=<tenths> word=<4 hex>      round(7.984 * f_Hz), big-endian',
         '# PLDEC word=<4 hex> dhz=<tenths>      snapped to the standard list where one matches',
         '# PLMAP model=<m> tone=<off> list=<off> count=<off> mode=<off> dec=<off> max=<n>',
         '# PLDENC / PLDDEC: the MCEZ13 DECODER law, round(61.107 * f_Hz), 0 where absent']
    for i, d in enumerate(PL_TONES):
        L.append('TONE idx=%d dhz=%d word=%04x' % (i, d, pl_encode(d)))
    for d in PL_TONES[1:]:
        L.append('PLENC dhz=%d word=%04x' % (d, pl_encode(d)))
        L.append('PLDEC word=%04x dhz=%d' % (pl_encode(d), d))
    # non-standard values must still round-trip through the law, just without snapping
    for w in (0x0300, 0x0500, 0x07FF):
        L.append('PLDEC word=%04x dhz=%d' % (w, pl_decode(w)))
    for m in sorted(PL):
        p = PL[m]
        L.append('PLMAP model=%s tone=%03x list=%03x count=%03x mode=%03x dec=%03x max=%d'
                 % (m, p['tone'], p['list'], p['count'] or 0, p['mode'] or 0,
                    p.get('dec') or 0, p['max']))
    # the decoder law, on the tones the original itself lists
    for d in PL_TONES[1:]:
        L.append('PLDENC dhz=%d word=%04x' % (d, pl_dec_encode(d)))
        L.append('PLDDEC word=%04x dhz=%d' % (pl_dec_encode(d), d))
    open(os.path.join(OUT, 'pl', 'pl.vec'), 'w').write('\n'.join(L) + '\n')
    return len(L)


def vec_header():
    """P-3 address framing, including nibbles the captures never exercise.

    Every address in both captures is 64-byte aligned with a zero top nibble, so nibbles 0 and 3
    are constant there and a transposed encoder replays perfectly.  These vectors pin all four.
    """
    L = ['# command header vectors, spec P-3',
         '# HDR cmd=<3 chars> addr=<4 hex> want=<7 hex>',
         '# 3 command characters then 4 nibble-characters of address, high nibble first,',
         '# each 0x30 + nibble']
    for cmd in (')40', ')01', '(40'):
        for a in (0x0000, 0x0040, 0x00C0, 0x0100, 0x01C0, 0x0200,
                  0x000F, 0x1234, 0xABCD, 0xFFFF):
            want = cmd.encode() + bytes(0x30 + ((a >> sh) & 15) for sh in (12, 8, 4, 0))
            L.append('HDR cmd=%s addr=%04x want=%s' % (cmd, a, want.hex()))
    open(os.path.join(OUT, 'proto', 'header.vec'), 'w').write('\n'.join(L) + '\n')
    return len(L)


def vec_parity():
    """7O1-over-8N1 codec, spec P-2."""
    L = ['# software parity vectors, spec P-2',
         '# TX in=<byte> out=<byte>     odd parity of bits 0-6 placed in bit 7',
         '# RX in=<byte> ok=<0|1> val=<byte>']
    for b in range(0x80):
        odd = 1 - (bin(b).count('1') & 1)
        L.append('TX in=%02x out=%02x' % (b, b | (odd << 7)))
    for b in (0x00, 0xFF, 0x30, 0xB0, 0x06, 0x86, 0x15, 0x95, 0x2A, 0xAA):
        good = bin(b).count('1') & 1 == 1
        L.append('RX in=%02x ok=%d val=%02x' % (b, 1 if good else 0, b & 0x7F))
    open(os.path.join(OUT, 'parity', 'parity.vec'), 'w').write('\n'.join(L) + '\n')
    return len(L)


def vec_trace(src, name, note):
    """Normalise a per-byte capture into a replay fixture."""
    ev, t = [], 0
    for line in open(os.path.join(ROOT, src), 'rb').read().decode('latin1').split('\n'):
        mm = re.match(r'\s*(\d+)\s*-\s*(pc|radio)\s*:\s*([0-9a-f]+)', line)
        if mm and int(mm.group(3), 16) <= 0xFF:
            t += int(mm.group(1))
            ev.append((t, mm.group(2), int(mm.group(3), 16)))
    # drop the leading line-state noise: everything before the first '(' or ')' or '*'
    start = next(i for i, (_, w, b) in enumerate(ev) if w == 'pc' and b in (0x28, 0x29, 0x2A))
    ev = ev[start:]
    runs, cur, who, t0 = [], [], None, None
    for tt, w, b in ev:
        if w != who:
            if cur:
                runs.append((who, t0, tlast, bytes(cur)))
            cur, who, t0 = [], w, tt
        cur.append(b); tlast = tt
    if cur:
        runs.append((who, t0, tlast, bytes(cur)))
    L = ['# replay trace, generated from %s' % src,
         '# %s' % note,
         '# <dir> <seq> <t_first_ms> <t_last_ms> <hex>   dir: TX = PC->radio, RX = radio->PC',
         'TRACE %s' % name]
    for i, (w, a, b, data) in enumerate(runs):
        L.append('%s %-3d %-6d %-6d %s' % ('TX' if w == 'pc' else 'RX', i, a - runs[0][1],
                                           b - runs[0][1], data.hex()))
    open(os.path.join(OUT, 'traces', name + '.trace'), 'w').write('\n'.join(L) + '\n')
    return len(runs)


def vec_trace_split(pc_src, trx_src, name, note):
    """Interleave a 2009-vintage split capture into the same run format.

    That capture is two files -- everything the PC sent, everything the radio sent -- with no
    timestamps and no interleaving, so the pairing is reconstructed from the protocol itself: each
    command consumes the next reply.  Times are emitted as 0 because the capture has none; the
    replay tests skip timing assertions for this trace and take them from the 2011 logs.
    """
    pc = open(os.path.join(ROOT, pc_src), 'rb').read()
    trx = open(os.path.join(ROOT, trx_src), 'rb').read()
    runs, i, j, pend = [], 0, 0, bytearray()

    def reply():
        nonlocal j
        if trx[j:j + 3] in (b'(01', b'(02'):
            n = 9 + (2 if trx[j:j + 3] == b'(02' else 0)
        elif trx[j:j + 3] == b'(40':
            n = 8 if trx[j + 7:j + 8] == b'\x15' else 135
        elif 0x30 <= trx[j] <= 0x3F:                      # bare nibble run: the ident
            n = 0
            while j + n < len(trx) and 0x30 <= trx[j + n] <= 0x3F:
                n += 1
        else:
            raise SystemExit('unparsed reply at %d: %r' % (j, trx[j:j + 8]))
        r = trx[j:j + n]
        j += n
        return r

    while i < len(pc):
        c = pc[i]
        if c == 0x06:                                     # ACK rides in front of the next command
            pend.append(c); i += 1; continue
        if c == 0x2A:                                     # identify
            pend.append(c); i += 1
        elif c == 0x29:                                   # a read/probe command
            pend += pc[i:i + 7]; i += 7
        elif c == 0x28:                                   # a write command
            pend += pc[i:i + 135]; i += 135
        else:
            raise SystemExit('unparsed command at %d: %r' % (i, pc[i:i + 8]))
        runs.append(('pc', bytes(pend))); pend = bytearray()
        runs.append(('radio', reply()))
    if pend:
        runs.append(('pc', bytes(pend)))
    L = ['# replay trace, interleaved from %s and %s' % (pc_src, trx_src),
         '# %s' % note,
         '# <dir> <seq> <t_first_ms> <t_last_ms> <hex>   dir: TX = PC->radio, RX = radio->PC',
         '# this capture carries NO timing; both time columns are 0',
         'TRACE %s' % name]
    for k, (w, data) in enumerate(runs):
        L.append('%s %-3d %-6d %-6d %s' % ('TX' if w == 'pc' else 'RX', k, 0, 0, data.hex()))
    open(os.path.join(OUT, 'traces', name + '.trace'), 'w').write('\n'.join(L) + '\n')
    return len(runs)


EDITS = [
    ('fixtures/eva9_real.bin', 'eva_sel5',
     [('set_tx', 1, 145_000_000), ('set_rx', 1, 160_000_000), ('set_tx', 3, 433_500_000),
      ('flag:decode', 1, 0), ('flag:decode', 1, 1), ('flag:encode', 1, 1),
      ('flag:power_high', 1, 0), ('flag:clock_shift', 1, 1), ('flag:tx_inhibit', 2, 1)]),
    ('samples/MCMICR70.DAT', 'eva_56',
     [('set_tx', 1, 435_000_000), ('flag:encode', 1, 1), ('flag:clock_shift', 5, 1)]),
    ('fixtures/eza9_programmed.bin', 'eza_sel5',
     [('set_tx', 1, 145_000_000), ('set_rx', 1, 160_000_000),
      ('flag:clock_shift', 1, 1), ('flag:decode', 1, 0), ('flag:auto_ack', 1, 0),
      ('flag:power_high', 1, 1)]),
    ('fixtures/ez13_default_band2.bin', 'eza_cspl',
     [('set_tx', 1, 145_000_000), ('flag:clock_shift', 1, 1), ('flag:clock_shift', 1, 0),
      ('flag:reserved_b7', 1, 1),
      # channel 5's record is zeroed, so its inverted clock-shift bit reads ON; programming it
      # must leave clock shift OFF (K-24a)
      ('set_tx', 5, 145_000_000)]),
]

SAMPLES = [
    ('samples/MCMICR70.DAT', 'eva_56'), ('samples/MCMICR2M.DAT', 'eva_56'),
    ('fixtures/eva9_real.bin', 'eva_sel5'), ('fixtures/ev9_default.bin', 'eva_sel5'),
    ('fixtures/eza9_default_band2.bin', 'eza_sel5'),
    ('fixtures/eza9_programmed.bin', 'eza_sel5'),
    ('fixtures/ez13_default_band2.bin', 'eza_cspl'),
]

if __name__ == '__main__':
    for d in ('aak', 'codeplug', 'edit', 'freq', 'parity', 'pl', 'proto', 'traces'):
        os.makedirs(os.path.join(OUT, d), exist_ok=True)
    for path, model in SAMPLES:
        n = vec_codeplug(path, model)
        print('  codeplug/%-22s %d lines' % (os.path.basename(path).split('.')[0].lower() + '.vec', n))
    print('  freq/roundtrip.vec        %d lines' % vec_freq())
    print('  edit/edits.vec            %d lines' % vec_edit())
    print('  pl/pl.vec                 %d lines' % vec_pl())
    print('  aak/aak.vec               %d lines' % vec_aak())
    print('  proto/header.vec          %d lines' % vec_header())
    print('  parity/parity.vec         %d lines' % vec_parity())
    for src, name, note in [
            ('captures/log_read_eva9.txt', 'read_eva9',
             'identify via `*`, size probe to a header-NAK, then a write and a re-identify'),
            ('captures/log_write_eva9.txt', 'write_eva9',
             '8 write blocks, each with the double ACK ~710 ms apart (spec P-25)')]:
        print('  traces/%-24s %d runs' % (name + '.trace', vec_trace(src, name, note)))
    print('  traces/%-24s %d runs' % ('read_eva9_2009.trace', vec_trace_split(
        'captures/mcm_read_eva9_pc.txt', 'captures/mcm_read_eva9_trx.txt',
        'read_eva9_2009',
        'a clean read: identify, 8 blocks, header-NAK; reconstructs fixtures/eva9_real.bin')))
    # manifest
    lines = []
    for root, _, files in os.walk(OUT):
        for f in sorted(files):
            if f.endswith(('.vec', '.trace')):
                p = os.path.join(root, f)
                lines.append('%s  %s' % (hashlib.sha256(open(p, 'rb').read()).hexdigest(),
                                         os.path.relpath(p, OUT)))
    open(os.path.join(OUT, 'MANIFEST.sha256'), 'w').write('\n'.join(sorted(lines)) + '\n')
    print('  MANIFEST.sha256           %d files' % len(lines))
