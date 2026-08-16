# MCprog

A modern programmer for the **Motorola MC micro** land-mobile radio family (EVA and EZA), reading
and writing the same `.DAT` codeplugs as the original 1987 Radio Service Software.

The MC micro is still in wide amateur use, and the only way to configure one has been a DOS program
that fails on any PC built after about 1994. MCprog replaces it: same codeplug format, same wire
protocol, no DOS.

```
$ mcprog samples/MCMICR70.DAT            # edit a codeplug file
$ mcprog --port /dev/ttyUSB0             # read the radio, then edit it
$ mcprog --port /dev/ttyUSB0 --identify
ident: EV9.01.00.11 455M11-3     5/6 Tone radio
$ mcprog --port /dev/ttyUSB0 --read mine.dat --log session.trace
ident: EV9.01.00.11 455M11-3     5/6 Tone radio
read 512 bytes (8 records)
2 models fit (eva_56, eva_sel5); assuming eva_56 -- use --model to choose
checksum valid
wrote mine.dat
```

One binary. A radio and a file are two sources of the same thing, so either leads to the same
editor; naming an output file makes it non-interactive instead, which is what a script or a bug
report wants.

## Status

| | |
|---|---|
| codeplug library | five models, decode and edit |
| protocol library | replays all three hardware captures byte for byte |
| TUI | channel list, per-channel editor, save with checksum |
| serial transport | POSIX (tested) and Win32 (compiles, **never run on hardware**) |
| reading a real radio | **untested** — needs the interface cable |
| writing a real radio | not implemented; needs W-1..W-6 first |

Nothing here has yet touched a physical radio. Everything is verified against captured hardware
sessions and against the original software running under emulation.

## Supported models

| model | size | channels | notes |
|---|---|---|---|
| `eva_56` | 512 B | 32 | MCEV 5/6-tone |
| `eva_sel5` | 512 B | 32 | MCEV9 / MCEV9M, SEL5 |
| `eza_sel5` | 256 B | 8 | MCEZ9, SEL5 |
| `eza_cspl` | 128 B | 8 | MCEZ13, carrier squelch / PL |

## Build

C99, no dependencies except ncurses for the TUI.

```
make          # build mcprog
make check    # conformance suite, pty smoke test, Windows cross-compile
```

Windows via MSYS2/mingw-w64. `make win-check` cross-compiles the portable core and the Win32
transport from a Unix host; the TUI needs curses, so it is built under MSYS2.

## How it is verified

The reverse engineering behind this is not in this repository — what is here is the *evidence*, in a
form that can be re-run.

* **`spec.md`** is the normative contract. Requirements are numbered (`P-n` protocol, `K-n`
  codeplug, `U-n` interface, `W-n` write safety) and every statement carries its provenance:
  **[C]** measured, **[S]** from the disassembly, **[?]** assumed. Tests cite the numbers.
* **`testdata/`** is language-neutral: line-oriented, integer hertz and milliseconds, lowercase
  hex, parseable with `fgets`+`strtok` or `bufio.Scanner` with no dependency either side. No
  expected value is written in the test source, so a second implementation runs the identical
  suite and any disagreement localises to one vector. `testdata/gen.py` regenerates it all;
  the output is committed and regeneration is byte-identical.
* **`--log`** writes the wire in exactly the `.trace` format `testdata/` consumes, so a session
  with a real radio drops straight into the conformance suite. If you are testing this against
  hardware, that log is the single most useful thing to send back.
* **`captures/`** holds two independent recordings of the original software talking to a real
  radio, from 2009 and 2011. `traces/read_eva9_2009.trace` replayed through the protocol library
  reconstructs `fixtures/eva9_real.bin` byte for byte.
* **Mutation testing.** Every law is deliberately broken to confirm the suite notices. That found
  four real holes that green tests had hidden: the EZA record layout (both EZA fixtures are factory
  defaults whose channels are all zero, so TX@+0 and RX@+3 were indistinguishable), the address
  nibbles (every address in every capture is 64-byte aligned and below 0x1000, leaving two nibbles
  constant), channel state (the vector format recorded none), and `CS8` (a pty is 8-bit clean
  whatever `CSIZE` says, so the loopback cannot see a port that would strip the parity bit).

## The traps

Reading the spec is worth it before changing anything, but these are the ones that bite:

* **The write handshake sends two ACKs.** The first means "accepted", the second, ~710 ms later,
  means "burned". Proceeding on the first desynchronises the radio on the very next record.
* **Frequency encoding is not canonical.** `b2` is a full byte but `P` ≤ 254, so `b2 >= P` is a
  legal alternate spelling — and the original emits some. Encode canonically, decode permissively,
  and **verify by decoded frequency, never by bytes**.
* **The channel table is terminated, not sparse.** Stop at the first record whose `+0` is `0xFF`.
  Records past it are stale and must be written back untouched.
* **Channel flag bits are not portable between models.** Bit 3 is clock shift on the EVA and
  auto-acknowledge on the EZA 9; the EZA 9 splits bit 7 by half; the EZA 1/3 clock shift is stored
  *inverted*.
* **The ident comes from `*`, not `)01`.** `)01` returns a single codeplug byte. Getting this
  backwards yields a tool that cannot identify a radio.

## Licence

GPL-3.0-or-later. See [COPYING](COPYING).

## Provenance

MCprog is an independent implementation. It contains no Motorola code.

`captures/` are recordings of a protocol session; `samples/` are user codeplug files; `fixtures/`
are codeplug images, most of them factory defaults recovered by driving the original software's own
initialise-to-default function under emulation. `fixtures/README.md` gives the provenance of each.
