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
"""Drive the real ncurses binary through a pty and check what it does.

The M3 criteria are behavioural, so they are checked against the actual UI rather than against the
library underneath it.  Run from the repository root after `make`:

    python3 tests/tui_smoke.py
"""
import os, pty, select, shutil, signal, sys, time

BIN = os.path.abspath('build/mcprog')
if not os.access(BIN, os.X_OK):
    sys.exit('%s is not built -- run `make` first' % BIN)
TMP = '/tmp/mc_tui_smoke.DAT'
ESC, CR = b'\x1b', b'\r'
ok = bad = 0


def check(cond, what):
    global ok, bad
    if cond:
        ok += 1
    else:
        bad += 1
        print('FAIL  %s' % what)


def drive(keys, src, model=None, settle=0.30):
    shutil.copyfile(src, TMP)
    before = open(TMP, 'rb').read()
    argv = [BIN] + (['--model', model] if model else []) + [TMP]
    pid, fd = pty.fork()
    if pid == 0:
        os.environ.update(TERM='xterm', LINES='40', COLUMNS='100')
        os.execv(BIN, argv)
        os._exit(1)
    out = b''

    def pump(t):
        nonlocal out
        end = time.time() + t
        while time.time() < end:
            if select.select([fd], [], [], 0.05)[0]:
                try:
                    out += os.read(fd, 65536)
                except OSError:
                    return
    pump(settle)
    for k in keys:
        os.write(fd, k)
        pump(settle)
    try:
        os.kill(pid, signal.SIGTERM)
        os.waitpid(pid, os.WNOHANG)
    except (ProcessLookupError, ChildProcessError):
        pass
    os.close(fd)
    return before, open(TMP, 'rb').read(), out.decode('latin1', 'replace')


def screen(out):
    """Render the stream through a simplified terminal.

    Use this only where LAYOUT matters.  For "did the program display X", assert against the raw
    stream instead: ncurses redraws partially and differently per implementation -- Apple's ncurses
    and Linux ncursesw emit different sequences for the same screen -- and ansi.py does not model
    every one, so a correctly drawn field can be missing from the render.
    """
    sys.path.insert(0, 'tests')
    from ansi import Screen
    return Screen(100, 40).feed(out.encode('latin1')).text()


# The very first keypress must do its job, not be swallowed dismissing the model-detection note.
_, _, out = drive([CR], 'samples/MCMICR70.DAT')
check('TX frequency' in out, 'Enter as the first key opens the channel editor')
check('models fit' in out or 'detected' in out, 'and the detection note is still shown')

# M3's done-criterion: an edit-and-save changes only the edited field and the checksum.
before, after, _ = drive([b' ', CR, CR, b'145.0000', CR, ESC, b's', b' ', b'q'],
                         'samples/MCMICR70.DAT')
diff = {i for i in range(len(before)) if before[i] != after[i]}
check(diff and diff <= {0x000, 0x0E2, 0x0E3, 0x0E4},
      'edit+save touched only the TX field and the checksum, got %s' % sorted(map(hex, diff)))
check(sum(after) & 0xFF == 0xFF, 'checksum valid after save')
dec = lambda t, P: ((((t[0] & 3) << 8) | t[1]) * P + t[2]) * (3125 if t[0] & 4 else 2500)
check(dec(after[0xE2:0xE5], 254) == 145_000_000, 'the stored TX decodes back to 145.000000 MHz')
check(before[0xE2] & 0xF8 == after[0xE2] & 0xF8, 'K-22: the flag bits in b0 survived the edit')

# U-3: not representable must be refused, never clamped, and nothing written.
before, after, out = drive([b' ', CR, CR, b'433.5000', CR], 'fixtures/eva9_real.bin')
check(before == after, 'U-3: a rejected frequency leaves the file untouched')
check('not representable' in out, 'U-3: the refusal says why')

# U-1 / K-24: neither unprogrammed nor stale rows may render as 0.00000.
_, _, out = drive([b' '], 'fixtures/eva9_real.bin')
check('171.23750' in out and '166.43750' in out, 'channel 1 shows its golden TX/RX')
check('terminator' in out, 'the end of the channel table is marked')
check('0.00000' not in out, 'K-24: no row is ever written as 0.00000 MHz')

# K-14: the PL page must write the tone and the mode byte, and nothing else.
# eva9_real starts in selectable, so mode cycles selectable -> off -> single; then down to the
# tone row and type it.
# j/k rather than the arrow keys: ncurses reads a lone ESC here rather than assembling the
# escape sequence, which would exit the page instead of moving down.
DOWN = b'j'
before, after, out = drive([b' ', b'p', CR, CR, DOWN, CR, b'88.5', CR, ESC, b's', b' ', b'q'],
                           'fixtures/eva9_real.bin')
diff = {i for i in range(len(before)) if before[i] != after[i]}
check(diff <= {0x000, 0x047, 0x048, 0x1FD},
      'PL edit touched only the tone word, the mode byte and the checksum, got %s'
      % sorted(map(hex, diff)))
check(after[0x047] == 0x02 and after[0x048] == 0xC3,
      'PL 88.5 Hz stored as 02C3, exactly what the 1987 editor wrote')
check((after[0x1FD] & 0xF0) == 0x60, 'PL mode byte set to 0x60 (single tone)')
check(sum(after) & 0xFF == 0xFF, 'checksum still valid after a PL edit')

# MCEZ13 does have PL, and uniquely both directions.
_, _, out = drive([b' ', b'p'], 'fixtures/ez13_default_band2.bin')
check('encode' in out and 'decode' in out,
      'MCEZ13 PL page shows both an encoder and a decoder list')
check('67.0 Hz' in out, 'and the factory default reads 67.0 Hz, not a "no decode" sentinel')

# The EZA models have different flags entirely (K-22).
_, _, out = drive([b' ', CR], 'fixtures/ez13_default_band2.bin')
check('clock_shift' in out, 'MCEZ13 exposes clock_shift')
check('reserved_b7' in out, 'and the bit it preserves but never exposes')
check('decode' not in out and 'encode' not in out,
      'K-22: MCEZ13 has no per-channel encode/decode flags')

print('\n%d passed, %d FAILED' % (ok, bad))
sys.exit(1 if bad else 0)
