# MCprog

Programmer for the **Motorola MC micro** land-mobile family (EVA and EZA). Reads and writes the
same `.DAT` codeplugs, over the same wire protocol, as the 1987 DOS Radio Service Software.

One binary. A file and a radio are two sources of the same object and both open the same editor;
naming an output file makes the run non-interactive.

## Synopsis

```
mcprog file.DAT                                 edit a codeplug file
mcprog --port DEV                               read the radio, then edit it
mcprog --port DEV --read f.DAT                  read to a file and exit
mcprog --port DEV --identify                    print the radio's ident
mcprog --port DEV --enable-write                read, edit, 'w' writes back
mcprog --port DEV --write f.DAT --enable-write  write a file to the radio
mcprog --selftest report.md                     probe a real radio and write a report (dev aid)
mcprog --dump-vec file.DAT                      conformance decode of a file
mcprog --list-models                            models compiled into this build
```

| option | effect |
|---|---|
| `--model NAME` | override model detection |
| `--log FILE` | record the wire in `.trace` format (with `--port`) |
| `--backup FILE` | name the pre-write backup; default is a timestamped file in the working directory, never overwritten (W-2) |
| `--baud N` | default 1200; `0` leaves the port speed alone |
| `--no-line-setup` | skip the 1.8 s DTR/RTS opening sequence |
| `--enable-write` | permit writing; without it no write path exists (W-1) |

### Control lines

Neither line is modem control and there is no handshake. Required states, and why:

| line | required state | RS-232 level | reason |
|---|---|---|---|
| DTR | **de-asserted** | negative | supplies the interface's level shifter. DTR drives a BC557 (PNP) through 27 k, which conducts only with its base pulled negative. |
| RTS | **asserted** | positive | wired straight through to the radio's HUB/PGM input. A positive level there selects programming mode. |

A control line sits at its negative level when de-asserted, so `MCR=0` is what puts negative on DTR.
The opening sequence (P-12) is `MCR=0`, 500 ms, assert RTS, 1300 ms — total 1.8 s, which
`--no-line-setup` skips.

Measured on hardware, 17 Aug 2026: **RTS asserted is what decides whether the radio answers.** It
replied on `DTR=0 RTS=1` *and* `DTR=1 RTS=1`, and was silent on both combinations with RTS
de-asserted, so the DTR polarity was not what settled it — the negative-rail reading is from the
schematic and the interface builder, not from a measurement at the shifter.

**De-asserting RTS ends the session.** After a probe dropped RTS, that radio never spoke again —
not to the following probe, nor to a fresh session that re-asserted RTS and waited the full 1.8 s.
Power-cycle before retrying. One run, one radio; the mechanism is not established.

## Models

| model | size | channels | radios | notes |
|---|---|---|---|---|
| `eva_56` | 512 B | 32 | EVA 9, 5/6-tone signalling | twelve radio-wide timers (`o`); no write counter — `0x0AF` is a programming date |
| `eva_sel5` | 512 B | 32 | EVA 9, SEL5 signalling | same timer table; write counter `0x0AF` |
| `eza_sel5` | 256 B | 8 | MCEZ9 + R/M builds | auto-ack delay `0x076`; write counter `0x09E` |
| `eza_cspl` | 128 B | 8 | MCEZ13 | PL scale 7.9844, not 7.984; no write counter |

The two 512-byte models are the **same hardware** — EVA 9 — differing only in which signalling the
set is fitted with. Nothing in a 512-byte file distinguishes them: same size, same checksum rule,
same channel layout. Detection reports the ambiguity and assumes `eva_sel5`; that is a preference,
not a determination. Override with `--model`.

**Reading from a radio removes the guess.** The ident names the signalling — the one real EVA ident
on record is `EV9.01.00.11 455M11-3     5/6 Tone radio` — so a radio read that sees `5/6 Tone`
selects `eva_56` and says the ident decided. There is no SEL5 marker on record, so its absence
proves nothing and falls back to the preference.

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
`-D_DEFAULT_SOURCE`. Darwin must **not** get `_POSIX_C_SOURCE`, which hides `cfmakeraw` and
`openpty` there, so the define is conditioned on `uname`. `pkg-config` is used when present to
locate an `ncursesw`-only distro's headers; without it the build falls back to `-lncurses`.

Built and tested on macOS, Debian 13 (GCC 14.2 / glibc 2.41) and Ubuntu 24.04. The Windows path is
cross-compiled from a Unix host and has never been built or run on Windows.

## Status

| component | state |
|---|---|
| codeplug library | four models; decode, edit, checksum |
| protocol library | replays all three recorded hardware sessions byte for byte (four files: the 2009 read is a PC/TRX pair) |
| TUI | channel list, per-channel editor, PL, options, timers, save |
| serial transport | POSIX — used against a real radio 17 Aug 2026; Win32 compiles, never run |
| write path | gated, backed up, verified per record (W-1..W-6) |
| read from a real radio | **done** — one EZA 9, 256 bytes, every mapped field as predicted |
| write to a real radio | **never attempted** — nothing has ever been written to a radio |

Everything not marked above is verified against captured hardware sessions, a fake radio on a pty,
and the original software under emulation.

## Running without a radio

`build/ptyserv` presents a radio on a pseudo-terminal; the whole tool including the write path runs
against it.

```
make build/ptyserv
./build/ptyserv fixtures/eva9_real.bin      # prints e.g. /dev/ttys004
./build/mcprog --port /dev/ttys004 --no-line-setup --baud 0 --enable-write
```

`--baud 0` and `--no-line-setup` are required only because a pty has neither a line speed nor
control lines.

## Verification

The reverse engineering is not in this repository; the *evidence* is, in re-runnable form.

| artefact | purpose |
|---|---|
| `spec.md` | Normative contract. Numbered requirements — `P-n` protocol, `K-n` codeplug, `U-n` interface, `W-n` write safety — each carrying provenance: **[C]** measured, **[S]** disassembly, **[?]** assumed. Tests cite the numbers. |
| `testdata/` | Language-neutral vectors: line-oriented, integer Hz and ms, lowercase hex. No expected value appears in test source, so a second implementation runs the identical suite and any disagreement localises to one vector. `testdata/gen.py` regenerates byte-identically. |
| `captures/` | Two independent recordings of the original software against a real radio, 2009 and 2011. `traces/read_eva9_2009.trace` replayed through the protocol library reconstructs `fixtures/eva9_real.bin` exactly. |
| `--log` | Writes the wire in the `.trace` format `testdata/` consumes, so a real session drops straight into the conformance suite. |

**Mutation testing.** Every law is deliberately broken to confirm the suite notices. That found four
holes green tests had hidden:

- EZA record layout — both EZA fixtures are factory defaults with all-zero channels, so TX@+0 and
  RX@+3 were indistinguishable;
- address nibbles — every address in every capture is 64-byte aligned and below `0x1000`, leaving
  two nibbles constant;
- channel state — the vector format recorded none;
- `CS8` — a pty is 8-bit clean whatever `CSIZE` says, so loopback cannot detect a port that would
  strip the parity bit.

## Implementation notes

Read `spec.md` before changing anything. These are the parts that bite.

**Write handshake sends two ACKs.** First means accepted, second (~710 ms later) means burned.
Proceeding on the first desynchronises the radio on the next record.

**Frequency encoding is not canonical.** `b2` is a full byte but `P` ≤ 254, so `b2 >= P` is a legal
alternate spelling and the original emits some. Encode canonically, decode permissively, verify by
decoded frequency — never by bytes.

**The channel table is terminated, not sparse.** Stop at the first record whose `+0` is `0xFF`.
Records past it are stale and must be written back untouched.

**Channel flag bits are not portable between models.** Bit 3 is clock shift on the EVA and
auto-acknowledge on the EZA 9; the EZA 9 splits bit 7 by half; the EZA 1/3 clock shift is stored
inverted.

**Timers round, they do not truncate,** and four of the twelve are narrower than the word they
occupy. Both facts come from the table the original itself walks. A two-point fit of the
synthesiser lock time is off by 1 ms, and the sample codeplug holds zeros exactly where the mask
bits would show.

**The write counter is four bits.** `0x1F` → `0x10`, not `0x20`: the low nibble counts, bit 4 is
*set* on wrap. Incrementing the whole byte corrupts bits 5–7. On `eva_56` that offset is not a
counter at all — it is the programming date.

**The PL tone scale is per-model.** EVA and EZ9 builds carry 7.984, MCEZ13 carries 7.9844. Exactly
one of the 39 EIA tones — 118.8 Hz — distinguishes them. Model field, not a `#define`.

**The ident comes from `*`, not `)01`.** `)01` returns a single codeplug byte. Reversing these
yields a tool that cannot identify a radio.

**Self-consistency is not correctness.** MCEZ13's map was two bytes low everywhere — model offsets,
golden vectors and fixtures all agreeing with each other, all derived from a capture that had been
stripped of two leading bytes to work around a malformed ident. Only the radio's own software
settled it.

## Selftest — for developing the tool against real hardware

`--selftest` is a development aid, not part of normal use. Editing a codeplug needs none of it.
It exists so that someone with a radio can answer questions the emulator cannot, and send back a
report: it probes the interface, measures timings, records what the radio does, and writes
everything down in a form this project can consume.

If you have a model that has never been read, this is the single most useful thing you can run.

```
mcprog --selftest report.md
```

- Finds the port itself: enumerates USB adapters first, then motherboard ports, uses whichever
  answers. On failure it prints an **ACTION REQUIRED** block, waits up to 60 s, and retries.
- Read-only. `--enable-write` additionally writes one record back — the radio's *own* bytes,
  unchanged, after saving a copy — exercising write framing and the double ACK without altering
  radio behaviour.
- Emits `report.md`, `report.md.trace` (timestamped wire log in conformance format) and
  `report.md.dat` (the codeplug read).
- First probe tries all four DTR/RTS combinations and reports which the radio answers on.
  Because dropping RTS can end the session, it stops at the first that answers and keeps that port
  open.

**Power-cycle the radio before each run.** On the first hardware run the radio stopped answering
after its programming mode was interrupted and did not recover within that session.

The first radio through it corrected four clauses of `spec.md` and exposed two bugs in the
selftest itself.

## Licence

GPL-3.0-or-later. See [COPYING](COPYING).

## Provenance

Independent implementation. Contains no Motorola code.

`captures/` are protocol recordings; `samples/` are user codeplug files; `fixtures/` are codeplug
images, most of them factory defaults recovered by driving the original software's initialise
function under emulation. `fixtures/README.md` gives per-file provenance.
