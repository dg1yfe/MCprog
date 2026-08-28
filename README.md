# MCprog

Programmer for the **Motorola MC micro** land-mobile family (EVA and EZA) and the **Radius M110**.
Reads, edits and writes the same `.DAT` codeplugs over the same wire protocol as the 1987 DOS Radio
Service Software.

C99, one binary, no runtime dependencies beyond ncurses. A file and a radio are two sources of the
same object and open the same editor. Naming an output file makes the run non-interactive.

Normative contract: **`spec.md`** — numbered requirements (`P-n` protocol, `K-n` codeplug,
`U-n` interface, `W-n` write safety) with provenance marks. Tests cite the numbers.

## 1. Usage

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
| `--force` | allow `--new` to overwrite an existing file |

Port naming: `/dev/ttyUSB0`, `/dev/ttyS0` on Linux; **`/dev/cu.usbserial-*`** on macOS, not
`/dev/tty.*`, which blocks on carrier detect. On Linux the user must be in the `dialout` group.

### Working without a radio

All file-side operation needs no radio. A codeplug can be created from nothing:

```
mcprog --list-defaults                          # what is available
mcprog --new eza_sel5 --band 3 new.DAT          # create it
mcprog new.DAT                                  # edit it
```

`--new` starts from a genuine factory default captured off the wire from the repair software's
`INITIALIZE`. No default exists for either Radius M110; `--new` refuses and lists the alternatives.
It refuses to overwrite an existing file without `--force`, and refuses combination with any option
that touches a radio.

### Storno CQM 5500

Storno resold these radios rebadged; the codeplugs and wire protocol are identical, so there is no
separate Storno model. `--list-models` shows Storno's own name against each MC micro model, and
`--model` accepts either.

```
mcprog --model "CQM5500 EZA 9, SELECT 5" file.DAT     # resolves to eza_sel5
mcprog --model "eza 1/3" file.DAT                     # a unique fragment is enough
mcprog --model "CQM5500" file.DAT                     # refused: matches several
```

`--list-models` also shows the ident each radio's original software demands. MCprog records it and
does not enforce it.

## 2. Status

| capability | state |
|---|---|
| Decode, edit, re-encode, checksum | six models — four MC micro, two Radius M110 |
| Read a codeplug from a radio | **works on hardware** |
| Write a codeplug to a radio | **works on hardware** — every record read back and verified |
| Protocol library | replays all three recorded hardware sessions byte for byte |
| TUI | channel list, per-channel editor, PL, options, timers, save |
| Serial transport, POSIX | used against a real radio |
| Serial transport, Win32 | compiles; never built or run on Windows |
| Test suite | at least 1,400 assertions, all passing |

Hardware-verified on an MC micro EZA 9 and four Radius M110s. Everything else is verified against
captured hardware sessions, a fake radio on a pseudo-terminal, and the original software under
emulation.

### What to expect on the wire

At 1200 baud:

| operation | EVA / EZA | Radius M110 |
|---|---|---|
| read, per 64-byte record | ~1.2 s | ~1.2 s |
| full 256-byte read | ~4.9 s | ~4.9 s |
| write, per record, accepted to burn-confirmed | ~0.7 s | **3.3–3.9 s** |

The M110's EEPROM burn is about five times slower than the EVA's.

A radio that stops answering needs a power cycle. Recovery behaviour may not be universal — no EVA
has been tested against it, so the in-band recovery path is retained.

## 3. Models

| name | bytes | channels | checksum | PL | radios |
|---|---|---|---|---|---|
| `eva_sel5` | 512 | 32 × 8 | `0x000` → 0xFF | encode | EVA, SEL5 signalling — MCEV9, MCEV9M |
| `eva_56` | 512 | 32 × 8 | `0x000` → 0xFF | encode | EVA, 5/6-tone signalling — MCEV_56 |
| `eza_sel5` | 256 | 8 × 6 | `0x000` → 0xFF | encode | EZA, SEL5 signalling — MCEZ9 and its R/M builds |
| `eza_cspl` | 128 | 8 × 6 | `0x003` → 0xFF | enc + dec | EZA, CS/PL — MCEZ13 |
| `m110_cspl` | 256 (128 real) | 10 × 10 | `0x00F` → **0x01** | enc + dec, per channel | **Radius M110** CSQ/PL, `EZ3.01.00.44` |
| `m110_sel5` | 256 | 9 × 12 | `0x00F` → **0x01** | — | **Radius M110** Sel 5, `EZ9.01.00.45` |

Detection is by size, checksum, and a marker in the bytes where the format has one. The M110 names
its family at `0x07..0x09` (`EZA` or `EZ9`).

The two 512-byte models are the same hardware differing only in signalling, so nothing in a *file*
separates them; detection assumes `eva_sel5`. From a radio the ident decides — `5/6 Tone` selects
`eva_56`. `--model` overrides detection but not the marker: a model contradicting the marker in the
bytes is refused.

| model | difference that matters |
|---|---|
| `eva_56` | no write counter; `0x0AF` is a programming date |
| `eza_sel5` | auto-ack delay at `0x076`; counter at `0x09E` |
| `eza_cspl` | PL scale 7.9844, not 7.984 — changes exactly one of the 39 EIA tones (118.8 Hz) |

### Radius M110

Answers the same wire protocol; nothing else is shared.

| property | value |
|---|---|
| checksum | sums to **`0x01`**, byte at `0x0F` |
| band | four bits at the bottom of `0x0A` |
| MC micro offsets | none lands on a real field |
| CSQ/PL device | returns 256 bytes = two identical 128-byte copies; the codeplug is the first 128, and only those are written |
| CSQ/PL PL | per channel, in the record — encode `+0`, decode `+5`; 123.0 Hz stores `982` (× 7.9844) and `7516` (× 61.107) |
| Sel 5 PL | none |
| ident length | 40 bytes, against the EVA's 41 |

**Not implemented, because not established:** the channel flag bits and the timer block. Channel
counts are the size of the region the table occupies.

## 4. Control lines

Neither line is modem control; there is no handshake.

| line | required state | RS-232 level | function |
|---|---|---|---|
| DTR | **de-asserted** | negative | supplies the interface's level shifter (BC557 PNP through 27 k, conducting only with its base negative) |
| RTS | **asserted** | positive | reaches the radio CPU's `#NMI`; the rising edge restarts programming mode |

Opening sequence: `MCR=0`, 500 ms, assert RTS, 1300 ms — 1.8 s total, skipped by `--no-line-setup`.
RTS asserted is what decides whether the radio answers.

De-asserting RTS for several seconds takes the radio out of programming mode for the rest of the
session.

## 5. Write safety

A write is refused unless every rule holds. Specified `W-1`..`W-6` in `spec.md`; each has tests
citing the number.

| | rule |
|---|---|
| W-1 | Writing requires `--enable-write`. Without it the action is visible but disabled. |
| W-2 | A full read is dumped to a backup before the first write byte. Failure to read or write it aborts. An existing backup is never overwritten — `-1`, `-2`, … is appended until the name is free. |
| W-3 | Pre-write gates, all fatal: valid checksum, band not 7, model and size match, and every byte differing from what the radio just returned is one MCprog itself writes. A write that would change nothing is refused. |
| W-4 | Every record is read back and verified. A mismatch aborts, naming record and offset. |
| W-5 | The write counter increments on radio writes only, never on file saves; the checksum is then recomputed. |
| W-6 | Records go out in order 0..N. |

## 6. Build

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

Built and tested on macOS, Debian 13 (GCC 14.2 / glibc 2.41) and Ubuntu 24.04.

### Running without a radio

`build/ptyserv` presents a radio on a pseudo-terminal. The whole tool, including the write path, runs
against it.

```
make build/ptyserv
./build/ptyserv fixtures/eva9_real.bin      # prints e.g. /dev/ttys004
./build/mcprog --port /dev/ttys004 --no-line-setup --baud 0 --enable-write
```

`--baud 0` and `--no-line-setup` are needed because a pty has neither line speed nor control lines.

### Checking a port without a radio

```
make hwprobe PORT=/dev/cu.usbserial-XXXX
```

Verifies control-line timing on a real port with nothing attached.

## 7. Selftest

Development aid, not part of normal use. Most useful against a model that has never been read.

```
mcprog --selftest report.md
```

| behaviour | detail |
|---|---|
| port | found automatically — USB adapters first, then motherboard ports. On failure prints an **ACTION REQUIRED** block, waits up to 60 s, retries |
| default | read-only |
| with `--enable-write` | writes one record back — the radio's own bytes, unchanged, after saving a copy |
| output | `report.md`, `report.md.trace` (wire log in conformance format), `report.md.dat` (codeplug read) |
| line probing | tries the DTR/RTS combinations, stops at the first that answers and keeps that port open |

**Power-cycle the radio before each run.**

Reports go to `reports/`, which is **not committed**: they contain radio serial numbers and user
codeplug contents.

## 8. Repository

| path | contents |
|---|---|
| `spec.md` | normative contract; numbered requirements with provenance marks |
| `testdata/` | language-neutral conformance vectors. No expected value appears in test source, so a second implementation runs the identical suite. `testdata/gen.py` regenerates byte-identically |
| `captures/` | protocol recordings of the original software against a real radio, 2009 and 2011 |
| `samples/` | user codeplug files |
| `fixtures/` | codeplug images, most of them factory defaults recovered from the original software under emulation — `fixtures/README.md` gives per-file provenance |

`--log` writes the wire in the `.trace` format `testdata/` consumes, so a real session drops straight
into the conformance suite.

## 9. Licence

GPL-3.0-or-later — see [COPYING](COPYING). Independent implementation; contains no Motorola code.
