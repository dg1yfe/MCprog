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

# K-15: the auto-acknowledge delay, on the one model whose offset is measured.
_, _, out = drive([b' ', b'o'], 'fixtures/eza9_default_band2.bin')
check('auto-acknowledge delay' in out, 'the options page shows the auto-acknowledge delay')
check('203 ms' in out, 'and the factory default reads 203 ms')

before, after, out = drive([b' ', b'o', CR, b'1000', CR, ESC, b's', b' ', b'q'],
                           'fixtures/eza9_default_band2.bin')
diff = {i for i in range(len(before)) if before[i] != after[i]}
check(diff == {0x000, 0x076}, 'editing it moves only 0x076 and the checksum, got %s'
      % sorted(map(hex, diff)))
check(after[0x076] == 64, 'and 1000 ms is stored as a count of 64')
check(sum(after) & 0xFF == 0xFF, 'checksum valid after the edit')

before, after, out = drive([b' ', b'o', CR, b'2000', CR], 'fixtures/eza9_default_band2.bin')
check(before == after, 'U-3: 2000 ms is out of range and nothing is written')
check('refused' in out, 'U-3: and the refusal says so')

# A model with no measured offset must say so rather than offer a field it cannot place.  MCEZ13 has
# neither the auto-acknowledge delay nor a timers table.
_, _, out = drive([b' ', b'o'], 'fixtures/ez13_default_band2.bin')
check('no radio-wide options' in out, 'a model without the field says so instead of guessing')

# K-16: the EVA timers, from the table the original itself walks.
_, _, out = drive([b' ', b'o'], 'fixtures/eva9_real.bin')
check('synth lock time' in out, 'the options page lists the EVA timers')
check('TX time-out' in out and 'emergency debounce' in out, 'all twelve, first to last')

# Editing one moves its own bytes and nothing else, and leaves the flags sharing the word alone.
# 0x0BC is the auto-reset word: 13 bits of time under three flags (enable, carrier override,
# forced reset), so it is the one to test preservation on.
before, after, out = drive([b' ', b'o'] + [DOWN] * 7 + [CR, b'12000', CR, ESC, b's', b' ', b'q'],
                           'fixtures/eva9_real.bin')
diff = {i for i in range(len(before)) if before[i] != after[i]}
check(diff <= {0x000, 0x0BC, 0x0BD}, 'editing auto reset moves only its word and the checksum, got %s'
      % sorted(map(hex, diff)))
check((after[0x0BC] << 8 | after[0x0BD]) & 0x1FFF == 1200, 'and 12000 ms is stored as 1200')
check(after[0x0BC] & 0xE0 == before[0x0BC] & 0xE0, 'K-30: the three flags above it are untouched')

before, after, out = drive([b' ', b'o'] + [DOWN] * 7 + [CR, b'12345', CR], 'fixtures/eva9_real.bin')
check(before == after, 'U-3: 12345 ms is not on the 10 ms grid, so nothing is written')
check('refused' in out, 'U-3: and the refusal says so')

# The EZA models have different flags entirely (K-22).
_, _, out = drive([b' ', CR], 'fixtures/ez13_default_band2.bin')
check('clock_shift' in out, 'MCEZ13 exposes clock_shift')
check('reserved_b7' in out, 'and the bit it preserves but never exposes')
check('decode' not in out and 'encode' not in out,
      'K-22: MCEZ13 has no per-channel encode/decode flags')

# ---- the radio cycle: read, edit, write back, with no file anywhere (M5 + M7) ----------------
# build/ptyserv serves fixtures/eva9_real.bin over a pty, so this exercises the same code path a
# real radio would, minus the wire itself.

RADIO = '/tmp/mc_tui_radio.bin'


def drive_radio(keys, src, enable_write=True, settle=0.35, final_wait=1.0):
    """Run the TUI against a fake radio on a pty.  Returns (before, after, output)."""
    import subprocess
    shutil.copyfile(src, RADIO)
    before = open(RADIO, 'rb').read()
    srv = subprocess.Popen(['build/ptyserv', RADIO, '120000'], stdout=subprocess.PIPE, text=True)
    dev = srv.stdout.readline().strip()
    time.sleep(0.4)
    argv = [BIN, '--port', dev, '--no-modem-init', '--baud', '0']
    if enable_write:
        argv.append('--enable-write')
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
    pump(2.0)  # the initial read of the whole codeplug
    for k in keys:
        os.write(fd, k)
        pump(settle)
    pump(final_wait)  # a write is eight records with a burn delay each; do not cut it off
    # SIGKILL and WNOHANG, never SIGTERM and a blocking wait: if the radio has gone away the client
    # can be stuck in a read that no signal it handles will interrupt, and the harness then hangs
    # instead of failing.
    try:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, os.WNOHANG)
    except (ProcessLookupError, ChildProcessError):
        pass
    os.close(fd)
    srv.terminate()          # SIGTERM makes ptyserv save its EEPROM before exiting
    try:
        srv.wait(timeout=5)
    except Exception:
        srv.kill()
        srv.wait(timeout=5)
    return before, open(RADIO, 'rb').read(), out.decode('latin1', 'replace')


if os.access('build/ptyserv', os.X_OK):
    for f in os.listdir('.'):
        if f.startswith('mcprog-backup'):
            os.remove(f)

    # W-1: without the opt-in the key is there and refuses, rather than silently doing nothing.
    # The status line is redrawn in pieces, so these assertions go through the renderer: the raw
    # stream splits "written and verified" across a cursor move.
    before, after, out = drive_radio([b' ', b'w'], 'fixtures/eva9_real.bin', enable_write=False)  # noqa
    check('disabled' in screen(out), 'W-1: w without --enable-write says writing is disabled')
    check(before == after, 'W-1: and the radio is untouched')

    # The whole point of M5+M7: read a radio, edit it, write it back, no file on disk at any stage.
    before, after, out = drive_radio(
        [b' ', CR, CR, b'171.2625', CR, ESC, b'w', b'y', CR], 'fixtures/eva9_real.bin',
        final_wait=8.0)
    diff = {i for i in range(len(before)) if before[i] != after[i]}
    # Not the whole phrase: the status line went from "writing -- do not disconnect" to "written
    # and verified", ncurses redrew only the tail after the shared "writ", and the full phrase
    # therefore appears contiguously in neither the stream nor ansi.py's render of it.
    check('and verified: 8 records' in out, 'W-4: the TUI writes the radio and says it verified')
    check(diff and diff <= {0x000, 0x0E2, 0x0E3, 0x0E4},
          'W-3: only the edited TX field and the checksum changed, got %s' % sorted(map(hex, diff)))
    check(sum(after) & 0xFF == 0xFF, 'K-2: the radio ends up with a valid checksum')
    dec = lambda t, P: ((((t[0] & 3) << 8) | t[1]) * P + t[2]) * (3125 if t[0] & 4 else 2500)
    check(dec(after[0xE2:0xE5], 80) == 171_262_500,
          'K-10: the radio holds the frequency that was typed')
    backups = [f for f in os.listdir('.') if f.startswith('mcprog-backup')]
    check(len(backups) == 1, 'W-2: exactly one backup was written, got %r' % backups)
    check(backups and open(backups[0], 'rb').read() == before,
          "W-2: and it holds the radio's contents from before the write")
    for f in backups:
        os.remove(f)

    # Answering anything but y must not write.
    before, after, out = drive_radio([b' ', CR, CR, b'171.2625', CR, ESC, b'w', b'n', CR],
                                     'fixtures/eva9_real.bin')
    check(before == after, 'W-1: declining the confirmation leaves the radio untouched')
    check('not written' in screen(out), 'W-1: and says so')
    check(not [f for f in os.listdir('.') if f.startswith('mcprog-backup')],
          'W-2: a declined write leaves no backup behind')
    # --selftest (M6, temporary).  It is the thing that will run against the first real radio, so
    # it has to work before it gets there; the fake radio cannot answer the P-11 line question, but
    # everything else runs.
    import subprocess
    shutil.copyfile('fixtures/eva9_real.bin', RADIO)
    srv = subprocess.Popen(['build/ptyserv', RADIO, '120000'], stdout=subprocess.PIPE, text=True)
    dev = srv.stdout.readline().strip()
    time.sleep(0.4)
    r = subprocess.run([BIN, '--port', dev, '--no-line-setup', '--baud', '0', '--enable-write',
                        '--selftest', '/tmp/mc_selftest.md'],
                       capture_output=True, text=True, timeout=300)
    srv.terminate()
    try:
        srv.wait(timeout=5)
    except Exception:
        srv.kill()
    rep = open('/tmp/mc_selftest.md').read() if os.path.exists('/tmp/mc_selftest.md') else ''
    check(r.returncode == 0, 'selftest exits 0 against a reachable radio')
    check('0 differ, 0 failed' in rep, 'selftest finds nothing wrong with the fake radio')
    for probe in ('P-20', 'P-21', 'P-22', 'P-24', 'P-41', 'P-25', 'P-42', 'K-20', 'K-2', 'K-10'):
        check(probe in rep, 'selftest reports %s' % probe)
    check('pseudo-terminal' in rep,
          'P-11 is skipped with the reason, not failed, on a port with no control lines')
    check('recorded' in rep, 'a probe with nothing to compare against is not called "as documented"')
    check(open(RADIO, 'rb').read() == open('fixtures/eva9_real.bin', 'rb').read(),
          'the selftest write-back leaves the radio byte-identical')
    check(os.path.exists('/tmp/mc_selftest.md.trace'), 'selftest writes a wire log')
    check(os.path.exists('/tmp/mc_selftest.md.dat'), 'selftest saves the codeplug it read')
else:
    print('SKIP  build/ptyserv is not built -- the radio cycle was not exercised')

print('\n%d passed, %d FAILED' % (ok, bad))
sys.exit(1 if bad else 0)
