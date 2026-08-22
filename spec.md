# MC micro programmer — normative specification

This is the contract both implementations satisfy. Requirements are numbered so tests can cite
them: `P-n` protocol, `K-n` codeplug, `U-n` user interface.

Provenance of every statement here is one of:
**[C]** measured against the original software or a hardware capture ·
**[S]** read from the disassembly, spot-checked ·
**[?]** assumed, and flagged.

The reverse-engineering evidence lives in `../doc/`. Where this file and `../doc/` disagree, the
disagreement is a bug in one of them — say so rather than picking one.

---

## 1. Wire encoding

**P-1 Nibble encoding.** Payload bytes travel as two characters, high nibble first, each
`0x30 + nibble`. So `0x9C` → `'9'`, `'<'` (0x39, 0x3C). Decoding accepts `0x30`–`0x3F` only. **[C]**

**P-2 Software parity.** The link is 1200 baud, 7 data bits, odd parity, 1 stop. Implementations
open the port as **8N1** and do parity themselves:

```
transmit:  out = (b & 0x7F) | (odd_parity(b & 0x7F) << 7)
receive:   if popcount(in) is even -> PARITY error;  value = in & 0x7F
```

A 7O1 frame and an 8N1 frame are both 10 bit times, so the line waveform is identical and the radio
cannot tell. Every protocol byte is ≤ 0x7F, so seven bits suffice. **[C]**

Rationale: USB-serial bridges vary in their handling of 7-bit modes; none of them vary in 8N1.

**P-3 Frame.** A command is **3 ASCII command characters** followed by **4 nibble-characters** of
16-bit address, high byte first. Write commands append 128 nibble-characters of payload. **[C]**

> Every address in all three captures is 64-byte aligned and below 0x1000, so the highest and
> lowest address nibbles are **always `0`** there. A transposed encoder replays perfectly. The
> `proto/header.vec` vectors pin all four nibbles independently; this was found by mutation, not by
> reading. **[C]**

**P-5 Line noise.** `0x00` and `0x01` bytes appear in the PC stream at the start of a capture and
again where the 2011 log changes phase. No protocol byte is below `0x06`, so a receiver must skip
them rather than treat them as a frame. Their origin is the capture rig or the line settling; it is
**not** established that the original software emits them. **[?]**

**P-4 Receive masking.** Received bytes are masked with `0x7F` before interpretation. The accepted
set is `0x06` ACK, `0x15` NAK, `0x28` `(`, `0x29` `)`, `0x2A` `*`, `0x30`–`0x3F`. Anything else is
`INVALID RESPONSE FROM UNIT`. **[C]**

## 2. Line setup

**P-10** 1200 baud, 8N1 (parity in software, P-2), no flow control. Concretely: `CS8`, `PARENB`
clear, `CSTOPB` clear, `CRTSCTS` clear, and `ISTRIP`/`INPCK`/`PARMRK`/`IXON`/`IXOFF` all clear —
`ISTRIP` in particular would strip the very bit P-2 puts there.

> A pty loopback is 8-bit clean whatever `CSIZE` says, so it **cannot** detect a port opened `CS7`;
> that would only show up against real hardware, as silent corruption. The settings are therefore
> asserted directly by reading the termios back, not inferred from a successful round trip. **[C]**
**P-11** **DTR de-asserted, RTS asserted.** Most USB adapters assert both on open; clear DTR
explicitly. **[S]**
**P-12** On open: `MCR=0`, wait 500 ms, assert RTS, wait 1300 ms. **[S]**

> **First hardware run, 17 Aug 2026 [C].** A real radio answered on `DTR=0 RTS=1` *and* on
> `DTR=1 RTS=1`, and was silent on both combinations with RTS de-asserted. So **RTS asserted is
> what matters** — consistent with it driving HUB/PGM — and the DTR polarity did not decide
> whether the radio replied. Two cables were tried; one worked, one was silent throughout.
>
> The same run showed something sharper: after the probe de-asserted RTS, **the radio never spoke
> again** — not to the remaining probe, and not to the session that followed, which re-asserted RTS
> and waited the full 1.8 s. Whatever the mechanism, taking RTS away appears to end the programming
> session for good. The selftest therefore stops at the first combination that answers and keeps
> that port open. **If a radio goes quiet, power-cycle it before retrying.** The mechanism itself is
> not established — one run, one radio. **[?]**
>
> `mcprog --port DEV --selftest report.md` is what produced this.
>
> Neither line is doing modem control, and nothing here is a handshake. In the interface of
> `doc/ANALYSIS.md` §3, **DTR supplies the level shifter's negative rail** and **RTS drives the
> radio's HUB/PGM input, which is what puts the radio into programming mode** -- reported by the
> user, who has built the interface, and consistent with the schematic: DTR drives a BC557 (a PNP,
> so it conducts when its base is pulled negative) through 27 k, and RTS runs straight through.
> An RS-232 line that is *de-asserted* sits at its negative level, so `MCR=0` is what puts a
> negative voltage on DTR. That reading reconciles P-12 with the hardware, but it has not been
> measured on a radio: if the shifter instead needs DTR **raised**, P-12 is wrong and the first
> hardware contact will show it. **[?]**

## 3. Commands

**P-20** `*` (0x2A) — **identify**. Returns the ident string nibble-encoded, terminated by `0x1A`.
**Its length varies by model**; `0x1A` is the terminator to trust, not a byte count. **[C]**

| radio | bytes | ident |
|---|---|---|
| EVA 9, 5/6-tone (2009 capture) | 41 | `EV9.01.00.11 455M11-3     5/6 Tone radio` |
| EZA 9 (hardware, 17 Aug 2026) | 37 | `EZ9.00.02.03 Copr,1987 Motorola GmbH` |

> This is *not* once per power-up. The EVA capture sends `*` twice, 36 s apart, and both are
> answered; the EZA 9 answered four times in one session, including after a full 256-byte read, and
> all four replies were identical. Never cache the assumption. **[C]**

**P-21** `)01`+addr — **attention / probe**. Returns `(01`+addr + **one byte**, `eeprom[addr]`. The
capture's two calls return `0x36` then `0x73` — different values, so plainly codeplug data. **[C]**

**P-22** `)02`+addr — returns `(02`+addr + **two** bytes: `eeprom[addr]` and `eeprom[addr+1]`. It
appears in none of the three captures, so it was a guess from the disassembly until an EZA 9
answered it: `)020000` returned `FB 02`, and `)010000` / `)010001` on the same radio returned `FB`
and `02`. It is a two-byte read, not a serial-number command. **[C]**

> Still **never gate a write on it** — a full pre-write read is strictly stronger, and W-3 does
> exactly that.

**P-23** `)40`+addr — read 64 bytes, reply `(40`+addr + 128 nibble-characters. During a sequential
read every record after the first is requested with a leading `0x06`, the acknowledgement of the
previous one — confirmed on hardware, where all four EZA 9 records after the first carried it.
**[C]**

> A record takes about 1.25 s at 1200 baud: 135 characters out and back, plus the turnaround. A
> whole 256-byte EZA 9 read measured 5015 ms. **[C]**

**P-24** Past the end of memory the radio NAKs, in one of **two forms — both real**:

| form | seen on |
|---|---|
| echoed 7-byte header, then `0x15` | the EVA captures |
| a bare `0x15` | an EZA 9, on hardware (17 Aug 2026) |

Accepting only one misparses the end of every read on the other. This is not a spec ambiguity to be
resolved but a difference between radios, so accept both. **[C]**

**P-25** `(40`+addr+128 chars — write 64 bytes. The reply is **two bare ACK bytes, no header**: the
first ~130 ms after the last data byte (command accepted), the second **~710 ms** later (EEPROM burn
complete). Measured across all 8 blocks of the write capture, consistent to 3 ms. **[C]**

> **P-25 is the single easiest requirement to get wrong.** An implementation that proceeds on the
> first ACK desynchronises on the very next block. Never send block N+1 before ACK2.

**P-26 Acknowledgement is contextual.** A `0x06` ACK follows a *read* record, and it is transmitted
**in front of the next command**, not as a message of its own — the wire shows `06 29 34 30 30 30 34
30`. Two rules, both measured:

* during a **sequential read**, every record is acknowledged;
* a **standalone read**, such as the read-back that verifies a just-written record, is **not**
  acknowledged at all.

Both captures agree, and the write capture's eight verification reads carry no ACK. **[C]**

## 4. Timing

**P-30** 190 ms per received byte. Measured margin: the worst inter-byte gap *within* a reply is
43 ms and the median is 8 ms, matching the 8.33 ms character time. **[C]**

**P-31** 800 ms for the write's second reply, measured from completion of transmission. Note that
`tcdrain`/`FlushFileBuffers` on a USB bridge does not reliably mean "on the wire", so matching the
observed 130/710 ms exactly is not achievable — the *ordering* requirement of P-25 is what is
absolute. **[S]**

**P-32** Exactly **one** retry, of the attention phase only. **No retry** around read, write or
verify — fail loudly. An automatic retry mid-write is how a glitch becomes a half-written EEPROM.
**[S]**

## 5. Sessions

**P-40 Identify.** The opening exchange in both read captures is `)01 0000` → `*` → `)01 0000`:
probe, identify, probe. A write session opens with the probe alone. **[C]**

**P-41 Read all.** For `addr = 0, 0x40, 0x80, …`: send `)40`+addr; on a valid header read 128 payload
characters and ACK with `0x06`; on NAK (either form, P-24) stop. Device size = records × 64. Cap the
walk at 64 records. Verify the checksum (K-2) and **report** a failure without refusing the data.

> Not hypothetical: the 2011 capture's radio returns a codeplug whose checksum is `0x80`, not
> `0xFF`. A tool that refuses invalid data cannot read that radio at all — which is exactly the
> radio whose owner most needs to read it. **[C]**

**P-42 Write all.** Per record: `(40`+addr+payload → wait ACK1 → **wait ACK2** → `)40`+addr read-back
→ compare. Comparison is byte-exact **except** channel frequency fields, which compare by **decoded
frequency** (K-11). Any real mismatch aborts immediately, naming record and offset.

## 6. Codeplug

**K-1 Image.** The codeplug is the device's EEPROM verbatim. `.DAT` files are exactly these bytes,
so old files load unchanged. **[C]**

**K-2 Checksum.** One byte is chosen so that the covered range sums to `0xFF` mod 256. **The byte
is per-model** (K-20); the extent is the whole device on every model, MCEZ13 included. The sum loop
at `CS:0x767E` runs `0 .. size-1` and `size` is 128 there — watched at runtime, not inferred. **[C]**

> **This corrects a documented law.** MCEZ13's checksum was recorded as covering "126 of 128, all
> but the last two bytes". It does not; that reading came from a fixture whose two leading bytes had
> been stripped, which turned a 128-byte sum into a 126-byte one over a shifted array and moved
> every MCEZ13 offset down by two. The strip was compensating for a malformed synthetic ident
> rather than for anything the radio does — `../doc/EEPROM_MAP_EZA.md` has the whole chain. MCEZ13's
> checksum byte is **`0x003`**: the editor zeroes it and stores the complement there, watched at
> `CS:0x7B15` and `CS:0x7B34`.

**K-10 Frequency.** Each 3-byte field:

```
coarse = ((b0 & 3) << 8) | b1
step   = 3125 Hz if (b0 & 4) else 2500 Hz
freq   = (coarse * P + b2) * step
```

The RX field holds the local oscillator; the displayed RX frequency is **field + 21.4 MHz** (the
first IF). `P` = 80, 80, 128, 254 for bands 1–4. Band index 7 means unprogrammed — ask the user,
do not error. **[C]**

**K-11 Encoding is not canonical.** `b2` is a full byte but `P` ≤ 254, so any `b2 ≥ P` is an
alternate spelling of a lower-`b1` value, and the original emits some (2 of 388 sample fields).
Encoders MUST produce `b2 < P`; decoders MUST accept `b2 ≥ P`; **verification MUST compare decoded
frequencies, not bytes.** **[C]**

**K-12 Tones and durations.** `tone = Round(f_Hz × k)`, `duration = Round(t_ms × 8.208)`, both
big-endian. These are literal Turbo Pascal reals in the original (`83 68 91 ED 7C 7F` and
`84 D9 CE F7 53 03`). **[C]**

**The scale `k` is not the same on every model.** Every EVA and EZ9 build carries **7.984**; every
MCEZ13 build carries **7.9844** (`83 C5 6D 34 80 7F`, ten occurrences each, none mixed), and so
does the M110 RSS — as an IEEE double, that software being a later toolchain. The hardware figure
is `150 × 2^16 / (4.9248 MHz / 4)` = 7.98441, so 7.9844 is the *truer* of the two and 7.984 is the
1987 authors' four-significant-figure rounding of it. Of the 39 EIA tones exactly one separates
them — **118.8 Hz**, 948 against 949 — which is why the split went unnoticed for so long. Carry `k`
per model (`mc_model.pl_k`, in units of 1/10000); do not hard-code either value. **[C]**

**K-13 Signalling-format tone tables are COPIED, never computed.** The per-format tables have an
internal scale of 5.28 which appears nowhere in the original software — it only copies them. An
implementation that synthesises them from 5.28 will be wrong. **[C]**

**K-14 PL / CTCSS.** Radio-wide on every model that has it, so it is not a channel field. Tones are
`round(k × f_Hz)` big-endian — the same law, and the same per-model `k`, as K-12. The encoding is **lossy**: 88.5 Hz stores
as 707, which decodes to 88.55. Decoding therefore **snaps to the standard tone list**, 40 entries
in tenths of a Hz that the original software itself carries (EVA image, `CS:0x3436`, index 0 = 0.0
meaning no PL). Without the snap the tool shows 88.6 where the operator typed 88.5. **[C]**

| model | single tone | selectable list | count | mode |
|---|---|---|---|---|
| EVA (both) | `0x047` | `0x047 + 2i` | `0x0CE` high nibble | `0x1FD` |
| EZA 9 | `0x02F` | `0x031 + 2i` | `0x083` high nibble | `0x07F` |
| EZA 1/3 | — | — | — | — |

Mode byte: `0x60` = single tone, `0xE0` = selectable (the operator picks from the list at the
radio). The low nibble of the mode byte and of the count byte are not understood and must be
preserved (K-30) — the count's low nibble is a selectable-lockout marker. Range is 67.0–250.3 Hz,
or 0 to disable; anything else is refused, never rounded (U-3). **[C]**

**K-15 Auto-acknowledge delay.** One byte, `round(ms / 15.625)` — a count of 1/64 second — range
1–127, i.e. 16–1984 ms. Radio-wide. Only the EZA 9 map has it, at `0x076`. Bit 7 is never set by
the original software and its meaning is unknown, so it is preserved (K-30) and the value is the
low seven bits. Values outside 16–1984 ms are refused, never clamped (U-3). **[C]**

> Measured in both directions against `MCEZ9R`: seven values written and read back through the
> radio, six planted and displayed, exact at every point including the software's own stated bounds
> of 16 and 1984 ms; 2000 ms is refused and writes nothing. See `../doc/EEPROM_MAP_EZA.md`.
>
> **Only the repair build exposes this field.** The standard and master builds never ask for it,
> which is why it went unmapped for so long — and why "the editor never mentions that byte" is
> evidence about the *build*, not about the byte (`../doc/BUILD_VARIANTS.md`). What the radio's
> firmware does with `0x076` is inferred, not shown: the firmware is internal mask ROM. **[?]**

> **MCEZ13 is deliberately absent.** Its PL decoder and encoder tables at `0x010` and `0x024` are
> known, but whether they are indexed per channel is not established, and the model's read is still
> blocked (see `../doc/EEPROM_MAP_EZA.md`). A programmer that guessed here could write a codeplug
> no radio wants. It exposes nothing until that is settled.
>
> **The EZA 9 nearly went the same way.** An earlier pass concluded it had no PL at all, having
> tested only the base build — the one of four where the menu entry is inert. `MCEZ9R` and both
> `MCEZ9M` builds prompt for a frequency. Check every build before concluding a feature is absent.

**K-16 Radio-wide timers.** The original's `T' sub-screen, twelve fields at `0x0B3`–`0x0C4`. It
does not compute them one at a time. It walks **four parallel word arrays of twelve entries** —
`(offset, mask, A, B)` — through a single reader, and every EVA build carries that table byte for
byte identical (`MCEV_56` `0xC9B3`, `MCEV9` `0x8C5D`), which is why both 512-byte models get it.
The reader's law is

```
v       := ((cp[off] and $7F) shl 8 or cp[off+1]) and mask
display := Round(v * 10 / A) + B
```

**[S]**, and it agrees with twelve fields measured independently off the screen **[C]**.

| field | offset | mask | A | B | ms |
|---|---|---|---|---|---|
| RX/TX delay | `0x0B3` | `0x00FF` | 1 | 0 | `v × 10` |
| encoder pretime | `0x0B4` | `0x00FF` | 1 | 0 | `v × 10` |
| encoder hold time | `0x0B5` | `0x00FF` | 1 | 0 | `v × 10` |
| intersequence | `0x0B6` | `0x00FF` | 1 | 0 | `v × 10` |
| synth lock time | `0x0B7` | `0x00FF` | 12 | 10 | `round(v × 10 / 12) + 10` |
| TX time-out | `0x0B8` | `0x7FFF` | 1000 | 4 | `v × 10 + 4000` |
| rekey delay | `0x0BA` | `0x7FFF` | 1000 | 0 | `v × 10` |
| auto reset time | `0x0BC` | `0x1FFF` | 1000 | 0 | `v × 10` |
| ext alarm time | `0x0BE` | `0x3FFF` | 1000 | 0 | `v × 10` |
| emergency RX time | `0x0C0` | `0x7FFF` | 1000 | 0 | `v × 10` |
| emergency TX time | `0x0C2` | `0x7FFF` | 1000 | 0 | `v × 10` |
| emergency debounce | `0x0C4` | `0x007F` | 1 | 0 | `v × 10` |

Four things in that table are easy to get wrong:

* **It rounds, it does not truncate.** The division is a real one ending in the Turbo Pascal
  runtime's `Round` entry (`0x17CE` — the sibling at `0x17D2` is `Trunc`; they share a body and
  differ only in a flag that enables `inc ax` on the bit shifted out). An integer `div 12` fits the
  two shipped synth-lock values perfectly and is still wrong: it shows 80 and 176 where the radio
  shows 81 and 177. Three points refute it; two do not.
* **`A` is the unit, so `B` is too.** Six fields print seconds. `B = 4` on the TX time-out is four
  *seconds*, and the rekey delay's `A = 1000` is why the screen calls 10 ms of it `0 sec`.
* **The masks matter and the sample hides them.** Four fields are narrower than their storage, and
  in `MCMICR70.DAT` those extra bits happen to be zero. The real ones are not: `0x0B8` carries bit
  15, and `0x0BC` carries enable, carrier override and forced reset above its 13 bits. Preserve
  everything outside `mask` (K-30).
* **The original's own offsets are one byte low** for the five byte-wide fields — it always fetches
  a word and masks to `0x00FF`. That is the whole explanation for the stray read of `0x0B2` that
  this project chased for a while: nothing lives there; it is the high half of the RX/TX delay
  fetch.

MCprog stores milliseconds, which is what the codeplug holds, so it can be finer than the screen it
came from. A value the law cannot spell is refused, never rounded (U-3).

**K-20 Model descriptor.** These genuinely vary and must be data, not assumptions:

| model | size | checksum byte / extent | channels | band | ref dividers |
|---|---|---|---|---|---|
| `MCEV_56` 5/6-tone | 512 | 0x000 | 0x0E0, 32 × 8 | 0x0DC b4-6 | 0x0D4 |

> The two 512-byte models are the same hardware — EVA 9 — differing only in signalling, and nothing
> in the image separates them. Detection reports the ambiguity and prefers `eva_sel5`; that is a
> preference, not a determination. **The radio's ident settles it**: a real EVA names its
> signalling (`... 5/6 Tone radio`), so `mc_model_detect_ident()` selects `eva_56` on that marker.
> No SEL5 marker has ever been captured, so its absence concludes nothing. **[C]** for the marker,
> **[?]** for anything the absence might mean.
| `MCEV9` / `MCEV9M` SEL5 EVA | 512 | 0x000 | 0x0E0, 32 × 8 | 0x0DC b4-6 | 0x0D4 |
| `MCEZ9` SEL5 EZA | 256 | 0x000 | 0x0C8, 8 × 6 | 0x082 b4-6 | 0x0C4 |
| `MCEZ13` CS/PL | 128/256/512/1024 | **0x003**, whole device | 0x03B, 8 × 6 | 0x039 b4-6 | 0x004 |

**K-21 Channel record.** EVA: `+0` BCD number, `+1` trakmode, `+2..4` TX, `+5..7` RX. EZA: `+0..2`
TX, `+3..5` RX, with no number and no trakmode byte. **[C]**

**K-22 Channel flag bits are NOT portable between models.** The editor writes most flags into
**both** halves of the record; so must we. The full set, each pinned by driving the original editor
and diffing what it wrote (`../doc/EEPROM_MAP_EV9.md`, `../doc/EEPROM_MAP_EZA.md`):

| model | bit 3 | bit 4 | bit 5 | bit 6 | bit 7 |
|---|---|---|---|---|---|
| EVA (both) | clock shift | decode | TX inhibit | encode | RF power |
| EZA 9 | auto acknowledge | decode | TX inhibit | encode | clock shift (**RX half**), RF power (**TX half**, **[S]**) |
| EZA 1/3 | — | — | — | clock shift, **TX half, stored inverted** | preserved, never exposed **[S]** |

Three traps in one table: bit 3 means different things on the two families, EZA 9 splits bit 7 by
half, and the EZA 1/3 clock shift is stored **inverted** — the bit is *set* when the screen shows
`N`. The EVA row is pinned on the SEL5 build. `MCEV_56`'s own per-channel screen has
since been found and driven, and it exposes only TX frequency, RX frequency and clock shift — so
it corroborates bit 3 and says nothing about the rest. It also shows that the two builds differ in
*which half they read*: `MCEV9M` writes clock shift into bit 3 of both halves, `MCEV_56` displays
the TX half's bit 3 alone. Write both halves and the difference cannot bite you
(`../doc/EEPROM_MAP.md`). **[C]** MCEZ13 has no per-channel encode/decode/TX-inhibit at all; its PL lives in tables and TX
inhibit is a single global bit. **[C]** except as marked.

**K-23 The channel table is terminated, not sparse.** Stop at the first record whose `+0` is `0xFF`.
Records past the terminator are stale and **must be written back unchanged**. **[C]**

**K-24 Three channel states.** Beyond the terminator (does not exist), allocated but unprogrammed
(valid number, zero frequency), and programmed. Never render an unprogrammed slot as `0.00000 MHz`.
**[C]**

**K-24a Programming an empty slot defaults clock shift OFF.** An unprogrammed record's flag bits
are leftovers, not settings. That matters because MCEZ13 stores clock shift **inverted**, so a
zeroed record reads as clock shift *on*: in the factory default, channels 3–8 appear to have it
enabled purely because their records are zero, while channels 1–2 — the configured ones — do not.
Writing a frequency into such a slot would inherit that silently. So setting a frequency on a slot
whose state is `MC_CH_EMPTY` also clears clock shift; an already-programmed slot is left alone.
**[C]** for the inversion, **[?]** for the default, which is our choice and not the original's.

**K-30 Preserve verbatim, never compute**: reference dividers, radio type nibble, synthesiser lock
time, the low nibble of the band byte, write counter, serial number, and every byte no field owns.
A write must be able to justify each changed byte; refuse on any unexplained change. **[C]**

## 7. User interface

**U-1** Channel list, one row per channel, showing number, TX, RX and flags, with a visible marker
where the table terminates and stale rows dimmed below it (K-23).
**U-2** Selecting a channel opens a detail page with all its fields and brief help text.
**U-1a** Neither an unprogrammed slot nor a stale record may be rendered as a frequency. Stale
records are never decoded at all, so printing them yields `0.00000 MHz` unless explicitly
suppressed — a mistake this project made and caught in the rendered output. **[C]**

**U-3** Validation has three levels — OK, WARN (representable but outside the band, or an
unprogrammed slot), ERROR (not representable). **Never clamp silently.** Saving with WARNs is
allowed on confirmation; saving with ERRORs is refused.
**U-4** A protocol log page, always recording, exportable. This is the field-support tool.

## 8. Write safety

**W-1** Writing requires an explicit opt-in flag; absent it the action is visible but disabled, with
the reason shown.
**W-2** A full read of the radio is dumped to a backup file before the first write byte. Failure to
read or to write the backup aborts the write.

> The caller may name the file (`--backup`). The generated default is
> `mcprog-backup-<date>-<time>.dat` in the working directory, and **an existing backup is never
> overwritten**: the timestamp has one-second resolution, so two writes in the same second would
> otherwise land on one file and the second would destroy the only copy of what the radio held
> before the first. A `-1`, `-2`, … suffix is appended until the name is free.
**W-3** Pre-write gates, all fatal: checksum valid; band not 7; model and size match; every byte
that differs from what the radio just returned is one MCprog itself writes (K-30). A write that
would change nothing is refused rather than performed.

> The serial number is covered by the K-30 gate rather than by a check of its own: if it differs
> from the pre-write read, the write is refused along with every other unaccountable difference.
**W-4** Verify every record after writing (P-42). Abort on mismatch, naming the record, the offset
and the backup path.
**W-5** Increment the write counter on radio writes only, never on file saves, then recompute the
checksum. **Implemented**, per model, from measurement.

| model | counter | on write |
|---|---|---|
| `eva_sel5` | `0x0AF` | bits 0-3 count; bit 4 **set** on wrap; bits 5-7 preserved |
| `eza_sel5` | `0x09E` | the same, **and bit 7 cleared** |
| `eva_56` | — | keeps its programming *date* at `0x0AF`-`0x0B1`; no counter — see below |
| `eza_cspl` | — | not measured; that build refuses to write under emulation |

> Measured by chaining read-write cycles against the 1987 software and the simulated radio
> (`../tools/wcounter.py`). A read followed by a write with **no edit in between** moves exactly
> two bytes — this one and the checksum compensating for it (K-2) — and repeating with the
> emulated clock moved gives the identical diff, which is what separates a counter from a date.
>
> **It is not an eight-bit increment, and that matters.** `0x1F` becomes `0x10`, not `0x20`: on
> wrap the low nibble resets and bit 4 is *set*, so bit 4 reads as a sticky "reprogrammed more than
> fifteen times" flag rather than a fifth counter bit. Seventeen planted values pin it on the EVA
> and twelve on the EZA (`testdata/wcount/wcount.vec`). A programmer that adds one to the whole
> byte corrupts whatever bits 5-7 hold.
>
> The bump lands only in the bytes handed to the radio: `mc_write_radio` copies the image, bumps
> the copy and fixes its checksum, so the caller's codeplug — and therefore any file it saves — is
> byte-for-byte unchanged. It happens after every W-3 gate has passed, so a refused write never
> advances it.
>
> **The 5/6-tone build genuinely has none, and that is worth stating twice** because it is the
> surprising half: same radio, same 512-byte EEPROM, and the SEL5 build keeps a counter in the byte
> the 5/6-tone build keeps a year in. Behaviour says so (three chained writes on a fixed date move
> nothing) and so does the code — the byte-pair that reads the counter nibble, `push 0x00AF` then
> `push 0x000F`, is in **every** EV9 `.000` overlay and in **no** MCEV_56 file. Where MCEV_56
> touches `0x0AF` it writes the whole byte from a variable. Whether the radio's firmware reads
> either is unknown; it is mask ROM. **[?]**
>
> **`eva_56` is the trap.** `0x0AF` is a write counter on the SEL5 EVA and the *year* of the
> programming date on the 5/6-tone build — same offset, two 512-byte models MCprog cannot tell
> apart by size. Moving the emulator's clock proves it: 1987-03-15 writes `87 03 15`, 2026-12-31
> writes `C6 12 31`, the year being `((y-1900) div 10)*16 + ((y-1900) mod 10)`. MCprog writes
> neither on that model. **[C]**
>
> **MCEZ13 has none, and that is now measured rather than assumed.** It used to stop at `WRITING`
> with `ERROR : UNITS EXCHANGED`; with a correctly shaped ident and an unshifted codeplug it reads
> and writes end to end, and three chained cycles change **not one byte** — no counter, no date.
> **[C]**
**W-6** Record order 0..N faithful to the original. `--checksum-last` may be offered, but the
faithful order is the only one known to work on hardware.

> W-1 to W-4 and W-6 are implemented in `src/write.c` and asserted in `tests/test_write.c`, each
> gate proven by being made to fire. The whole path runs against the forked fake radio over a pty;
> **it has never run against a physical radio.** **[C]** for the code, **[?]** for the hardware.
