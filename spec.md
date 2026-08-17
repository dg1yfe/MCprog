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

> `mcprog --port DEV --selftest report.md` settles this empirically: it tries all four DTR/RTS
> combinations and reports which ones the radio answers on.
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
The captured EVA reply is 41 bytes: `EV9.01.00.11 455M11-3     5/6 Tone radio` + 0x1A. **[C]**

> This is *not* once per power-up. The capture sends `*` twice, 36 s apart, and both are answered.
> Never cache the assumption. **[C]**

**P-21** `)01`+addr — **attention / probe**. Returns `(01`+addr + **one byte**, `eeprom[addr]`. The
capture's two calls return `0x36` then `0x73` — different values, so plainly codeplug data. **[C]**

**P-22** `)02`+addr — serial-number pre-write check, returns `(02`+addr + **two** bytes. Appears in
**none of the three** captures. Implement it, but **never gate a write on it** — use a full
pre-write read, which is strictly stronger. **[S]**

**P-23** `)40`+addr — read 64 bytes, reply `(40`+addr + 128 nibble-characters. **[C]**

**P-24** Past the end of memory the radio replies with the **echoed 7-byte header and then `0x15`**.
A parser that accepts only a bare NAK misparses the end of every read. Accept both forms. **[C]**

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

**K-2 Checksum.** One byte is chosen so that the covered range sums to `0xFF` mod 256. **Both the
byte and the extent are per-model** (K-20): every model covers the whole device except **MCEZ13**,
which covers all but its **last two bytes** — 126 of 128. That was measured off the sum loop at
`CS:0x767E`, whose count comes from a variable, and confirmed by the comparison `cmp ax,0xff` that
follows it. Summing the whole device on MCEZ13 gives the wrong answer whenever those last two bytes
are non-zero. **[C]**

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

**K-12 Tones and durations.** `tone = Round(f_Hz × 7.984)`, `duration = Round(t_ms × 8.208)`, both
big-endian. These are literal Turbo Pascal reals in the original (`83 68 91 ED 7C 7F` and
`84 D9 CE F7 53 03`). **[C]**

**K-13 Signalling-format tone tables are COPIED, never computed.** The per-format tables have an
internal scale of 5.28 which appears nowhere in the original software — it only copies them. An
implementation that synthesises them from 5.28 will be wrong. **[C]**

**K-14 PL / CTCSS.** Radio-wide on every model that has it, so it is not a channel field. Tones are
`round(7.984 × f_Hz)` big-endian — the same law as K-12. The encoding is **lossy**: 88.5 Hz stores
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

> **MCEZ13 is deliberately absent.** Its PL decoder and encoder tables at `0x010` and `0x024` are
> known, but whether they are indexed per channel is not established, and the model's read is still
> blocked (see `../doc/EEPROM_MAP_EZA.md`). A programmer that guessed here could write a codeplug
> no radio wants. It exposes nothing until that is settled.
>
> **The EZA 9 nearly went the same way.** An earlier pass concluded it had no PL at all, having
> tested only the base build — the one of four where the menu entry is inert. `MCEZ9R` and both
> `MCEZ9M` builds prompt for a frequency. Check every build before concluding a feature is absent.

**K-20 Model descriptor.** These genuinely vary and must be data, not assumptions:

| model | size | checksum byte / extent | channels | band | ref dividers |
|---|---|---|---|---|---|
| `MCEV_56` 5/6-tone | 512 | 0x000 | 0x0E0, 32 × 8 | 0x0DC b4-6 | 0x0D4 |
| `MCEV9` / `MCEV9M` SEL5 EVA | 512 | 0x000 | 0x0E0, 32 × 8 | 0x0DC b4-6 | 0x0D4 |
| `MCEZ9` SEL5 EZA | 256 | 0x000 | 0x0C8, 8 × 6 | 0x082 b4-6 | 0x0C4 |
| `MCEZ13` CS/PL | 128/256/512/1024 | **0x001**, covers 126 | 0x039, 8 × 6 | 0x037 b4-6 | 0x002 |

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
`N`. MCEZ13 has no per-channel encode/decode/TX-inhibit at all; its PL lives in tables and TX
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
**W-2** A full read of the radio is dumped to a timestamped backup file before the first write byte.
Failure to read or to write the backup aborts the write.
**W-3** Pre-write gates, all fatal: checksum valid; band not 7; model and size match; every byte
that differs from what the radio just returned is one MCprog itself writes (K-30). A write that
would change nothing is refused rather than performed.

> The serial number is covered by the K-30 gate rather than by a check of its own: if it differs
> from the pre-write read, the write is refused along with every other unaccountable difference.
**W-4** Verify every record after writing (P-42). Abort on mismatch, naming the record, the offset
and the backup path.
**W-5** Increment the write counter on radio writes only, never on file saves, then recompute the
checksum.

> **Not implemented, deliberately.** The counter's offset differs per model and is only partly
> measured -- 0x0AF on the SEL5 EVA, 0x09E on the EZA 9, and on the 5/6-tone build a write moves
> 0x0B0 and 0x0B1 together, of which only one is plausibly a counter. Implementing it from a guess
> would write a byte MCprog cannot account for into somebody's radio, which is what W-3 exists to
> prevent. The cost of omitting it is that the radio does not record having been reprogrammed.
> **[?]**
**W-6** Record order 0..N faithful to the original. `--checksum-last` may be offered, but the
faithful order is the only one known to work on hardware.

> W-1 to W-4 and W-6 are implemented in `src/write.c` and asserted in `tests/test_write.c`, each
> gate proven by being made to fire. The whole path runs against the forked fake radio over a pty;
> **it has never run against a physical radio.** **[C]** for the code, **[?]** for the hardware.
