# MCprog

Programmer for the **Motorola MC micro** land-mobile family (EVA and EZA) and the **Radius M110**.
Same `.DAT` codeplugs and same wire protocol as the 1987 DOS Radio Service Software.

C99, one binary, no runtime dependencies beyond ncurses. A file and a radio are two sources of the
same object and open the same editor. Naming an output file makes the run non-interactive.

Normative contract: **`spec.md`**. Requirements are numbered `P-n` protocol, `K-n` codeplug,
`U-n` interface, `W-n` write safety, each carrying provenance — **[C]** measured, **[S]**
disassembly, **[?]** assumed. This file is a summary; `spec.md` is what tests cite.

## 1. Synopsis

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

All file-side operation needs no radio. A codeplug can be created from nothing:

```
mcprog --list-defaults                          # what is available
mcprog --new eza_sel5 --band 3 new.DAT          # create it
mcprog new.DAT                                  # edit it
```

| property | behaviour |
|---|---|
| source | a **genuine factory default**, captured off the wire from the repair software's `INITIALIZE`; byte-identical to what it produced in 1987 |
| why not a blank buffer | a codeplug holds many bytes this project has never mapped; an image built only from understood fields would be wrong undetectably and the radio would accept it |
| no default captured | both Radius M110s — `--new` says so and lists alternatives rather than inventing one |
| existing file | refused without `--force` |
| radio options | refused in combination |

### Storno CQM 5500

Storno resold these radios rebadged. Its programmer is the Motorola one **relocated, not rewritten**:
driven side by side, the two put byte-identical traffic on the wire. There is therefore no separate
Storno model. `--list-models` shows Storno's own name against each MC micro model and `--model`
accepts it.

```
mcprog --model "CQM5500 EZA 9, SELECT 5" file.DAT     # resolves to eza_sel5
mcprog --model "eza 1/3" file.DAT                     # a unique fragment is enough
mcprog --model "CQM5500" file.DAT                     # refused: matches several
```

`--list-models` also shows the ident each radio's *original* software demands. MCprog records it and
does not enforce it: a real EVA answers `EV9.01.00.11`, which the 1987 Standard build rejects as
`INVALID TYPE` while the Master and Repair builds of the same version accept.

## 2. Status

| capability | state |
|---|---|
| Decode, edit, re-encode, checksum | six models — four MC micro, two Radius M110 |
| Read a codeplug from a radio | **works on hardware** — EZA 9, 256 bytes, every mapped field as predicted |
| Write a codeplug to a radio | **never landed on hardware** — §3 |
| Protocol library | replays all three recorded hardware sessions byte for byte |
| TUI | channel list, per-channel editor, PL, options, timers, save |
| Serial transport, POSIX | used against a real radio |
| Serial transport, Win32 | compiles; never built or run on Windows |
| Test suite | at least 1,400 assertions, all passing |

Anything not marked hardware-verified is verified against captured hardware sessions, a fake radio on
a pseudo-terminal, and the original software under emulation.

## 3. Write status

No write has completed on a radio. The history is two distinct faults, one fixed and one open.

| date | outcome | cause |
|---|---|---|
| to 23 Aug 2026 | 8 sessions, 4 radios, all `no first ACK` | **MCprog** — `spec.md` P-31d |
| 28 Aug 2026 | record **accepted**; no burn confirmation; radio then silent | open — `spec.md` P-31e |

**P-31d.** `send()` returns when the kernel has *buffered* a frame, not when it has left. A write
frame is 135 bytes = **1125 ms** at 1200 baud; `MC_T_ACK1` was **400 ms**, so the window shut while
the radio was receiving byte 48 of 135. Reads (7 bytes, 58 ms) were never at risk — that asymmetry is
the signature. `drain()` now holds the ACK clock by waiting out the frame from the moment it was
handed to the kernel.

There is no portable way to ask whether the transmit register is empty. Measured on an FTDI FT232 at
1200 baud, 135-byte frame needing 1125 ms:

| mechanism | reported complete after |
|---|---|
| `tcdrain()` | **0.1 ms** |
| `TIOCOUTQ` (512-byte frame, 4267 ms needed) | **506 ms** |
| computed floor + margin | 1139 ms |

Waiting needs no driver, no ioctl and no cooperation from the adapter. Port-side checks, no radio
required:

```
make hwprobe PORT=/dev/cu.usbserial-XXXX
```

**P-31e.** `reports/write-runs3`, an `EZ3.01.00.44` M110: first ACK at **1140 ms** after the frame
was queued — where P-31d predicts — so the record was accepted. No second ACK inside `MC_T_BURN`,
then silence; the arming pulse did not recover it. `MC_T_BURN` was 2000 ms, an EVA figure (P-31a
derives 697 ms from `EZA33.BIN`) applied to firmware nobody has read; it is now **8000 ms**, 125 ms
per byte. Whether the liveness probes fired immediately afterwards contributed to the silence is
**not established**; the selftest now settles for a full burn timeout first.

Written records carry the radio's **own bytes**, so a partially committed burn writes back what was
already there.

Session recovery: the original software re-opens the line at the start of every operation, and
`mc_session_arm()` does the same — the P-27 sequence, twice, before a read and before a write.
`mc_serial_rearm()` remains as an explicit single pulse, used by the selftest's `P-24a` probe. That
behaviour may not be universal: a disassembled EVA firmware returns to its command loop after the
end-of-memory NAK rather than going deaf, and no EVA has been tested, so the recovery path stays.

## 4. Models

| name | bytes | channels | checksum | PL | radios |
|---|---|---|---|---|---|
| `eva_sel5` | 512 | 32 × 8 | `0x000` → 0xFF | encode | EVA, SEL5 signalling — MCEV9, MCEV9M |
| `eva_56` | 512 | 32 × 8 | `0x000` → 0xFF | encode | EVA, 5/6-tone signalling — MCEV_56 |
| `eza_sel5` | 256 | 8 × 6 | `0x000` → 0xFF | encode | EZA, SEL5 signalling — MCEZ9 and its R/M builds |
| `eza_cspl` | 128 | 8 × 6 | `0x003` → 0xFF | enc + dec | EZA, CS/PL — MCEZ13 |
| `m110_cspl` | 256 (128 real) | 10 × 10 | `0x00F` → **0x01** | enc + dec, per channel | **Radius M110** CSQ/PL, `EZ3.01.00.44` |
| `m110_sel5` | 256 | 9 × 12 | `0x00F` → **0x01** | — | **Radius M110** Sel 5, `EZ9.01.00.45` |

**Detection** is by size, checksum, and a marker in the bytes where the format has one. The M110
names its family at `0x07..0x09` (`EZA` or `EZ9`), a documented field from the 1989 software's
radio-type descriptor table, so it is identified positively rather than by elimination.

The two 512-byte models are the same hardware differing only in signalling, so nothing in a *file*
separates them; detection assumes `eva_sel5`. From a radio the ident decides — `5/6 Tone` selects
`eva_56`. There is no SEL5 marker, so its absence falls back to the default rather than concluding
anything.

`--model` skips detection but **not** the marker: a model contradicting the marker is refused, since
that is never resolving an ambiguity — it would read and write the wrong offsets.

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
| MC micro offsets | **none lands on a real field** — `eza_sel5`'s reference dividers read as zeros, its counter at `0x09E` is live channel data |
| CSQ/PL device | returns 256 bytes = **two identical 128-byte copies**; the codeplug is the first 128, only those are written, hence the 256-byte total of `0x02` |
| CSQ/PL PL | **per channel, in the record** — encode `+0`, decode `+5` |
| PL scales | 123.0 Hz stores `982` to encode (× 7.9844) and `7516` to decode (× 61.107); the 1989 RSS carries both as IEEE doubles |
| Sel 5 PL | none |

**Not established, so absent rather than guessed:** the channel flag bits and the timer block.
Channel counts are the size of the region the table occupies — inference from four radios that leave
the rest zero, not a measurement.

## 5. Control lines

Neither line is modem control; there is no handshake.

| line | required state | RS-232 level | function |
|---|---|---|---|
| DTR | **de-asserted** | negative | supplies the interface's level shifter (BC557 PNP through 27 k, conducting only with its base negative) |
| RTS | **asserted** | positive | reaches the radio CPU's `#NMI`; the rising edge restarts programming mode |

A line sits negative when de-asserted, so `MCR=0` puts negative on DTR. Opening sequence: `MCR=0`,
500 ms, assert RTS, 1300 ms — 1.8 s, skipped by `--no-line-setup`. RTS asserted decides whether the
radio answers; DTR polarity did not change the outcome in testing.

**A radio that has gone quiet needs a power cycle.** De-asserting RTS for several seconds took a
radio out of programming mode permanently within that session.

## 6. Write safety

A write is refused unless every rule holds. Specified `W-1`..`W-6` in `spec.md`; each has tests
citing the number.

| | rule |
|---|---|
| W-1 | Writing requires `--enable-write`. Without it the action is visible but disabled. |
| W-2 | A full read is dumped to a backup before the first write byte. Failure to read or write it aborts. An existing backup is never overwritten — `-1`, `-2`, … is appended until the name is free. |
| W-3 | Pre-write gates, all fatal: valid checksum, band not 7, model and size match, and every byte differing from what the radio just returned is one MCprog itself writes. A write that would change nothing is refused. |
| W-4 | Every record is read back and verified. A mismatch aborts, naming record and offset. |
| W-5 | The write counter increments on radio writes only, never on file saves; the checksum is then recomputed. |
| W-6 | Records go out in order 0..N, as the original does. |

## 7. Build

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
`nanosleep`, `clock_gettime`, and GCC 14 treats the implicit declarations as errors — hence
`-D_DEFAULT_SOURCE`. Darwin must **not** get `_POSIX_C_SOURCE`, which hides `cfmakeraw` and `openpty`
there, so the define is conditioned on `uname`. `pkg-config` locates an `ncursesw`-only distro's
headers when present; otherwise `-lncurses`.

Built and tested on macOS, Debian 13 (GCC 14.2 / glibc 2.41) and Ubuntu 24.04.

### Running without a radio

`build/ptyserv` presents a radio on a pseudo-terminal. The whole tool, including the write path, runs
against it.

```
make build/ptyserv
./build/ptyserv fixtures/eva9_real.bin      # prints e.g. /dev/ttys004
./build/mcprog --port /dev/ttys004 --no-line-setup --baud 0 --enable-write
```

`--baud 0` and `--no-line-setup` are needed only because a pty has neither line speed nor control
lines.

## 8. Selftest

Development aid, not part of normal use. Most useful against a model that has never been read.

```
mcprog --selftest report.md
```

| behaviour | detail |
|---|---|
| port | found automatically — USB adapters first, then motherboard ports. On failure prints an **ACTION REQUIRED** block, waits up to 60 s, retries |
| default | read-only |
| with `--enable-write` | writes one record back — the radio's *own* bytes, unchanged, after saving a copy — exercising framing and the double ACK without altering radio behaviour |
| output | `report.md`, `report.md.trace` (wire log in conformance format), `report.md.dat` (codeplug read) |
| line probing | tries the DTR/RTS combinations and reports which answered; stops at the first that answers and keeps that port open, because dropping RTS can end the session |

**Power-cycle the radio before each run.**

## 9. Verification

The reverse engineering is not in this repository; the *evidence* is, in re-runnable form.

| artefact | purpose |
|---|---|
| `spec.md` | Normative contract, numbered and provenance-marked. Tests cite the numbers. |
| `testdata/` | Language-neutral vectors: line-oriented, integer Hz and ms, lowercase hex. No expected value appears in test source, so a second implementation runs the identical suite and any disagreement localises to one vector. `testdata/gen.py` regenerates byte-identically. |
| `captures/` | Two independent recordings of the original software against a real radio, 2009 and 2011. `traces/read_eva9_2009.trace` replayed through the protocol library reconstructs `fixtures/eva9_real.bin` exactly. |
| `--log` | Writes the wire in the `.trace` format `testdata/` consumes, so a real session drops straight into the conformance suite. |

Every law in `spec.md` has been deliberately broken to confirm the suite notices.

## 10. Invariants for a second implementation

Read `spec.md` first. Most often got wrong:

| | invariant |
|---|---|
| Write handshake | **Two ACKs.** First = accepted, second (~710 ms on EVA) = burned. Proceeding on the first desynchronises the radio on the next record. |
| Frequency encoding | **Not canonical.** `b2` is a full byte but `P` ≤ 254, so `b2 >= P` is a legal alternate spelling and the original emits some. Encode canonically, decode permissively, compare by decoded frequency — never by bytes. |
| Channel table | **Terminated, not sparse.** Stop at the first record whose `+0` is `0xFF`. Records past it are stale and must be written back untouched. |
| Channel flag bits | **Not portable between models.** Bit 3 is clock shift on the EVA and auto-acknowledge on the EZA 9; the EZA 9 splits bit 7 by half; the EZA 1/3 clock shift is inverted. |
| Timers | **Round, do not truncate.** Four of the twelve are narrower than the word they occupy. |
| Write counter | **Four bits.** `0x1F` → `0x10`, not `0x20`: the low nibble counts, bit 4 is *set* on wrap. Incrementing the whole byte corrupts bits 5–7. |
| Ident | Comes from `*`, **not** `)01`. `)01` returns a single codeplug byte. Reversing the two yields a tool that cannot identify a radio. |

## 11. Licence and provenance

GPL-3.0-or-later — see [COPYING](COPYING). Independent implementation; contains no Motorola code.

| directory | contents |
|---|---|
| `captures/` | protocol recordings |
| `samples/` | user codeplug files |
| `fixtures/` | codeplug images, most factory defaults recovered by driving the original software's initialise function under emulation — `fixtures/README.md` gives per-file provenance |
