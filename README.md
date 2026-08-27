# MCprog

Programmer for the **Motorola MC micro** land-mobile family (EVA and EZA). Reads and writes the same
`.DAT` codeplugs, over the same wire protocol, as the 1987 DOS Radio Service Software.

One binary, no runtime dependencies beyond ncurses. A file and a radio are two sources of the same
object and both open the same editor; naming an output file makes the run non-interactive.

## Synopsis

```
mcprog file.DAT                                 edit a codeplug file
mcprog --port DEV                               read the radio, then edit it
mcprog --port DEV --read f.DAT                  read to a file and exit
mcprog --port DEV --identify                    print the radio's ident
mcprog --port DEV --enable-write                read, edit, 'w' writes back
mcprog --port DEV --write f.DAT --enable-write  write a file to the radio
mcprog --selftest report.md                     probe a radio and write a report
mcprog --dump-vec file.DAT                      conformance decode of a file
mcprog --list-models                            models compiled into this build
mcprog --new MODEL [--band N] FILE              create a codeplug from scratch, no radio needed
mcprog --list-defaults                          the factory defaults --new can create from
```

| option | effect |
|---|---|
| `--model NAME` | override model detection; also accepts a Storno CQM 5500 name, or any unique fragment of one |
| `--log FILE` | record the wire in `.trace` format (with `--port`) |
| `--backup FILE` | name the pre-write backup; default is a timestamped file in the working directory, never overwritten |
| `--baud N` | default 1200; `0` leaves the port speed alone |
| `--no-line-setup` | skip the 1.8 s DTR/RTS opening sequence (`--no-modem-init` is a legacy alias) |
| `--enable-write` | permit writing; without it no write path exists |

### Working without a radio

Everything file-side works with no radio attached — `mcprog FILE` edits, `--dump-vec FILE` decodes —
and a codeplug can be created from nothing:

```
mcprog --list-defaults                          # what is available
mcprog --new eza_sel5 --band 3 new.DAT          # create it
mcprog new.DAT                                  # edit it
```

`--new` starts from a **genuine factory default**, the way the repair software's `INITIALIZE` does,
not from a blank buffer. Those defaults were captured off the wire from that software, so what
`--new` writes is byte-identical to what it produced in 1987. That matters because a codeplug holds
many bytes this project has never mapped: an image assembled from only the understood fields would
be wrong undetectably, and the radio would accept it.

Where no default has been captured — both Radius M110s — `--new` says so and lists the alternatives
rather than inventing one. It refuses to overwrite an existing file without `--force`, and refuses
to be combined with any option that touches a radio.

### Storno CQM 5500

Storno resold these radios rebadged, and its programmer is the Motorola one **relocated, not
rewritten**. Driven side by side, the two put byte-identical traffic on the wire. So there is no
separate Storno model — `--list-models` shows Storno's own name against each MC micro model, and
`--model` accepts it:

```
mcprog --model "CQM5500 EZA 9, SELECT 5" file.DAT     # resolves to eza_sel5
mcprog --model "eza 1/3" file.DAT                     # a unique fragment is enough
mcprog --model "CQM5500" file.DAT                     # refused: matches several
```

`--list-models` also shows the ident each radio's *original* software demands. MCprog records it and
does not enforce it, because that rule is demonstrably too narrow: a real EVA answers
`EV9.01.00.11`, and the 1987 Standard build rejects it as `INVALID TYPE` while the Master and Repair
builds of the same version read it without complaint.

## What works

| capability | state |
|---|---|
| Decode, edit, re-encode, checksum | six models — four MC micro, two Radius M110 |
| Read a codeplug from a radio | **works on hardware** — EZA 9, 256 bytes, every mapped field as predicted |
| Write a codeplug to a radio | **never landed on hardware** — see below |
| Protocol library | replays all three recorded hardware sessions byte for byte |
| TUI | channel list, per-channel editor, PL, options, timers, save |
| Serial transport, POSIX | used against a real radio |
| Serial transport, Win32 | compiles; never built or run on Windows |
| Test suite | at least 1,400 assertions, all passing |

Everything not marked as hardware-verified is verified against captured hardware sessions, a fake
radio on a pseudo-terminal, and the original software under emulation.

### Writing has never succeeded on a radio

The write path is complete, gated and tested against a simulated radio, but no byte has ever reached
real hardware. Eight sessions across four radios failed with `no first ACK`, identically.

**The cause was this program, and it was arithmetic** (`spec.md` P-31d). `send()` returns when the
kernel has *buffered* the frame, not when it has left. A write frame is 135 bytes; at 1200 baud that
is **1125 ms on the wire**, and `MC_T_ACK1` is **400 ms** — so the window shut while the radio was
still receiving **byte 48 of 135**. No radio could have answered inside it. Every read in the same
sessions worked, because a read command is 7 bytes and takes 58 ms; that asymmetry is the signature.

`drain()` now holds the ACK clock until the frame can physically be gone, by **waiting out the frame
from the moment it was handed to the kernel**. There is no portable way to ask whether the transmit
register is really empty: `tcdrain` and `TIOCOUTQ` are what the platform offers and both report on
the kernel's own queue. On an FTDI FT232 at 1200 baud, where a 135-byte frame needs **1125 ms**,
`tcdrain()` returned after **0.1 ms** and `TIOCOUTQ` called a 4.3-second transmission finished after
**506 ms**. Waiting needs no driver, no ioctl and no cooperation from the adapter, and gives the same
answer on every port on every platform.

Run the port-side checks yourself, no radio needed:

    make hwprobe PORT=/dev/cu.usbserial-XXXX

Still untested on hardware. What that run must also settle is whether the radio fell silent after the
frame: the old traces cannot say, because discarded bytes were not logged. That is fixed too.

That behaviour may not be universal: a disassembled EVA radio firmware returns to its command loop
after sending the NAK rather than going deaf. No EVA has been tested, so the recovery path stays.

Recovery after a full read currently requires a power cycle. The original software never faces the
problem, because it re-opens the line at the start of every operation, and **mcprog now does the
same**: `mc_session_arm()` runs the sequence twice before a read and before a write, as the original
was measured doing (`spec.md` P-27). `mc_serial_rearm()` remains as an explicit single pulse, which
is what the selftest's `P-24a` recovery probe uses.

## Models

| name | bytes | channels | checksum | PL | radios |
|---|---|---|---|---|---|
| `eva_sel5` | 512 | 32 × 8 | `0x000` → 0xFF | encode | EVA, SEL5 signalling — MCEV9, MCEV9M |
| `eva_56` | 512 | 32 × 8 | `0x000` → 0xFF | encode | EVA, 5/6-tone signalling — MCEV_56 |
| `eza_sel5` | 256 | 8 × 6 | `0x000` → 0xFF | encode | EZA, SEL5 signalling — MCEZ9 and its R/M builds |
| `eza_cspl` | 128 | 8 × 6 | `0x003` → 0xFF | enc + dec | EZA, CS/PL — MCEZ13 |
| `m110_cspl` | 256 (128 real) | 10 × 10 | `0x00F` → **0x01** | enc + dec, per channel | **Radius M110** CSQ/PL, `EZ3.01.00.44` |
| `m110_sel5` | 256 | 9 × 12 | `0x00F` → **0x01** | — | **Radius M110** Sel 5, `EZ9.01.00.45` |

Detection is by size, checksum **and a marker in the bytes** where the format has one. The M110
names its family at `0x07..0x09` (`EZA` or `EZ9`) — a documented field of that format, from the 1989
software's own radio-type descriptor table — so it is identified positively rather than by
elimination.

The two 512-byte models are the same hardware differing only in signalling, so **nothing in a file
separates them**; detection assumes `eva_sel5`. From a radio the ident decides: `5/6 Tone` selects
`eva_56`. There is no SEL5 marker, so its absence falls back to the default rather than concluding
anything. Override with `--model`.

`--model` skips detection, but **not** the marker: naming a model that contradicts the marker in the
bytes is refused, because it is never resolving an ambiguity — it is a mistake that would read and
write the wrong offsets.

Per-model differences that matter: `eva_56` has no write counter (`0x0AF` is a programming date);
`eza_sel5` has an auto-ack delay at `0x076` and its counter at `0x09E`; `eza_cspl` uses PL scale
7.9844 rather than 7.984, which changes exactly one of the 39 EIA tones (118.8 Hz).

### The Radius M110 is a different radio

It answers the same wire protocol and nothing else is shared. Its checksum sums to **`0x01`**, not
`0xFF`, with the byte at `0x0F`; its band is four bits at the bottom of `0x0A`; and **no MC micro
offset lands on a real field** — `eza_sel5`'s reference dividers read as zeros and its write counter
at `0x09E` is live channel data on both families.

The CSQ/PL device returns 256 bytes that are **two identical 128-byte copies**. The codeplug is the
first 128, and only those are written — which is also why its 256-byte total is `0x02`: each half
sums to `0x01` independently.

Unlike every MC micro model, the CSQ/PL keeps **PL per channel**, in the record: encode at `+0`,
decode at `+5`. The two fields scale differently, so one tone stores as two numbers — 123.0 Hz is
`982` to encode (`× 7.9844`) and `7516` to decode (`× 61.107`). Both constants are ones MCEZ13
already uses; the 1989 RSS carries them as IEEE doubles, which pins the decoder constant that
measurement had only bounded. The Sel 5 has no PL.

What is **not** established, and so is absent rather than guessed: the channel flag bits and the
timer block. The channel counts are the size of the region the table occupies — inference from four
radios that leave the rest of it zero, not a measurement.

## Control lines

Neither line is modem control and there is no handshake.

| line | required state | RS-232 level | function |
|---|---|---|---|
| DTR | **de-asserted** | negative | supplies the interface's level shifter (a BC557 PNP through 27 k, conducting only with its base negative) |
| RTS | **asserted** | positive | reaches the radio CPU's `#NMI` input; the rising edge restarts programming mode |

A line sits at its negative level when de-asserted, so `MCR=0` is what puts negative on DTR. The
opening sequence is `MCR=0`, 500 ms, assert RTS, 1300 ms — 1.8 s total, which `--no-line-setup`
skips. RTS asserted is what decides whether the radio answers; the DTR polarity did not change the
outcome in testing.

**A radio that has gone quiet needs a power cycle.** De-asserting RTS for several seconds took a
radio out of programming mode permanently within that session.

## Write safety

A write is refused unless every one of these holds. They are specified as `W-1`..`W-6` in `spec.md`
and each has tests citing the number.

| | rule |
|---|---|
| W-1 | Writing requires `--enable-write`. Without it the action is visible but disabled. |
| W-2 | A full read is dumped to a backup file before the first write byte. Failure to read it, or to write it out, aborts. An existing backup is never overwritten — a `-1`, `-2`, … suffix is appended until the name is free. |
| W-3 | Pre-write gates, all fatal: valid checksum, band not 7, model and size match, and every byte differing from what the radio just returned is one MCprog itself writes. A write that would change nothing is refused rather than performed. |
| W-4 | Every record is read back and verified. A mismatch aborts, naming the record and offset. |
| W-5 | The write counter increments on radio writes only, never on file saves; the checksum is then recomputed. |
| W-6 | Records go out in order 0..N, as the original does. |

## Build

C99. No dependencies except ncurses for the TUI.

```
make            # build
make check      # conformance suite, pty smoke test, Windows cross-compile
```

| host | packages | notes |
|---|---|---|
| Linux | `build-essential`, `libncurses-dev` | Makefile adds `-D_DEFAULT_SOURCE`, links `-lutil` |
| Linux, `make check` | `python3`, `mingw-w64` | optional; each target skips if absent |
| macOS | Xcode CLT | ncurses ships with the SDK |
| Windows | MSYS2 / mingw-w64 | `make win-check` cross-compiles core + Win32 transport |

```
sudo apt install build-essential libncurses-dev python3 mingw-w64
```

`-std=c99` defines `__STRICT_ANSI__`; glibc then hides `strtok_r`, `cfmakeraw`, `openpty`,
`nanosleep` and `clock_gettime`, and GCC 14 treats the implicit declarations as errors — hence
`-D_DEFAULT_SOURCE`. Darwin must **not** get `_POSIX_C_SOURCE`, which hides `cfmakeraw` and `openpty`
there, so the define is conditioned on `uname`. `pkg-config` locates an `ncursesw`-only distro's
headers when present; otherwise the build falls back to `-lncurses`.

Built and tested on macOS, Debian 13 (GCC 14.2 / glibc 2.41) and Ubuntu 24.04.

## Running without a radio

`build/ptyserv` presents a radio on a pseudo-terminal. The whole tool, including the write path, runs
against it.

```
make build/ptyserv
./build/ptyserv fixtures/eva9_real.bin      # prints e.g. /dev/ttys004
./build/mcprog --port /dev/ttys004 --no-line-setup --baud 0 --enable-write
```

`--baud 0` and `--no-line-setup` are needed only because a pty has neither a line speed nor control
lines.

## Selftest

`--selftest` is a development aid, not part of normal use — editing a codeplug needs none of it. It
answers questions the emulator cannot and writes the answers in a form this project can consume. If
you have a model that has never been read, it is the most useful thing to run.

```
mcprog --selftest report.md
```

- Finds the port itself: USB adapters first, then motherboard ports, using whichever answers. On
  failure it prints an **ACTION REQUIRED** block, waits up to 60 s, and retries.
- Read-only by default. With `--enable-write` it also writes one record back — the radio's *own*
  bytes, unchanged, after saving a copy — exercising write framing and the double ACK without
  altering radio behaviour.
- Emits `report.md`, `report.md.trace` (wire log in conformance format) and `report.md.dat` (the
  codeplug read).
- Probes the DTR/RTS combinations and reports which the radio answers on. Because dropping RTS can
  end the session, it stops at the first that answers and keeps that port open.

**Power-cycle the radio before each run.**

## Verification

The reverse engineering is not in this repository; the *evidence* is, in re-runnable form.

| artefact | purpose |
|---|---|
| `spec.md` | Normative contract. Numbered requirements — `P-n` protocol, `K-n` codeplug, `U-n` interface, `W-n` write safety — each carrying provenance: **[C]** measured, **[S]** disassembly, **[?]** assumed. Tests cite the numbers. |
| `testdata/` | Language-neutral vectors: line-oriented, integer Hz and ms, lowercase hex. No expected value appears in test source, so a second implementation runs the identical suite and any disagreement localises to one vector. `testdata/gen.py` regenerates byte-identically. |
| `captures/` | Two independent recordings of the original software against a real radio, 2009 and 2011. `traces/read_eva9_2009.trace` replayed through the protocol library reconstructs `fixtures/eva9_real.bin` exactly. |
| `--log` | Writes the wire in the `.trace` format `testdata/` consumes, so a real session drops straight into the conformance suite. |

Every law in `spec.md` has been deliberately broken to confirm the suite notices.

## Notes for a second implementation

Read `spec.md` first. The invariants most often got wrong:

- **The write handshake sends two ACKs.** The first means accepted, the second (~710 ms later) means
  burned. Proceeding on the first desynchronises the radio on the next record.
- **Frequency encoding is not canonical.** `b2` is a full byte but `P` ≤ 254, so `b2 >= P` is a legal
  alternate spelling and the original emits some. Encode canonically, decode permissively, compare by
  decoded frequency — never by bytes.
- **The channel table is terminated, not sparse.** Stop at the first record whose `+0` is `0xFF`.
  Records past it are stale and must be written back untouched.
- **Channel flag bits are not portable between models.** Bit 3 is clock shift on the EVA and
  auto-acknowledge on the EZA 9; the EZA 9 splits bit 7 by half; the EZA 1/3 clock shift is inverted.
- **Timers round, they do not truncate**, and four of the twelve are narrower than the word they
  occupy.
- **The write counter is four bits.** `0x1F` → `0x10`, not `0x20`: the low nibble counts and bit 4 is
  *set* on wrap. Incrementing the whole byte corrupts bits 5–7.
- **The ident comes from `*`, not `)01`.** `)01` returns a single codeplug byte. Reversing the two
  yields a tool that cannot identify a radio.

## Licence

GPL-3.0-or-later. See [COPYING](COPYING).

## Provenance

Independent implementation. Contains no Motorola code.

`captures/` are protocol recordings; `samples/` are user codeplug files; `fixtures/` are codeplug
images, most of them factory defaults recovered by driving the original software's initialise
function under emulation. `fixtures/README.md` gives per-file provenance.
