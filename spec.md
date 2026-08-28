# MC micro programmer — normative specification

This is the contract both implementations satisfy. Requirements are numbered so tests can cite
them: `P-n` protocol, `K-n` codeplug, `U-n` user interface.

Provenance of every statement here is one of:
**[C]** measured against the original software or a hardware capture ·
**[S]** read from the disassembly, spot-checked ·
**[?]** assumed, and flagged.

References of the form `doc/…md`, `tools/…py` and `reports/run…` name files in the separate
reverse-engineering repository and in local hardware run records. Where this file and those notes
disagree, the disagreement is a bug in one of them.

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

**P-11** **DTR de-asserted, RTS asserted.** Most USB adapters assert both on open; clear DTR
explicitly. **[S]** — confirmed from the code: `ser_OpenLine` writes only `MCR=0x00` and `MCR=0x02`,
so DTR (bit 0) is never asserted by the original at all. That RTS reaches the radio CPU's `#NMI`
is **corroboration, not proof** — the NMI vector is where the disassembly puts the programming
entry, but **only a radio** can confirm the line actually drives that pin.

**P-12** `MCR=0`, wait 500 ms, assert RTS, wait 1300 ms — **before every transaction**, not once on
open. **[S]**

**P-27 The original runs that sequence TWICE, back to back, at the start of an operation — and not
again within it. MCprog does the same. [C]**
Measured as four line transitions, all at **TX byte zero**, for both read and write.

`mc_session_arm()` reproduces it, and `main.c` calls it at the head of the read and the write, so
the transport's `rearm` hook **is** reached from both paths. The pulse on `mc_serial_open()` was
removed: the 1987 software has no persistent open, and leaving it would make three pulses where the
original makes two. `mc_serial_rearm()` remains callable as a single explicit pulse, which is what
the selftest's `P-24a` probe uses.

Two consequences worth stating, because both are easy to get wrong:

* **`--no-line-setup` gates the automatic arming, not an explicit request.** The transport's `rearm`
  hook is left NULL when the flag is off, so `mc_session_arm()` does nothing — but a direct
  `mc_serial_rearm()` still pulses. The selftest depends on that: it sets `line_setup = 0` because
  it drives the lines itself, and `P-24a` must still be able to pulse deliberately.
* **Arming clears `pending_ack`.** The rising edge restarts the radio's programming routine, so no
  acknowledgement can be owed across it.

Why *twice* is **[?]**. It is reproduced because the object is to be identical, and a second NMI
costs nothing but time.

## 3. Commands

**P-20** `*` (0x2A) — **identify**. Returns the ident string nibble-encoded, terminated by `0x1A`.
**Its length varies by model**; `0x1A` is the terminator to trust, not a byte count. **[C]**

| radio | bytes | ident |
|---|---|---|
| EVA 9, 5/6-tone (2009 capture) | 41 | `EV9.01.00.11 455M11-3     5/6 Tone radio` |
| EZA 9 (hardware) | 37 | `EZ9.00.02.03 Copr,1987 Motorola GmbH` |

**P-21** `)01`+addr — **attention / probe**. Returns `(01`+addr + **one byte**, `eeprom[addr]`. The
capture's two calls return `0x36` then `0x73` — different values, so plainly codeplug data. **[C]**

**P-22** `)02`+addr — returns `(02`+addr + **two** bytes: `eeprom[addr]` and `eeprom[addr+1]`. It
appears in none of the three captures, so it was a guess from the disassembly until an EZA 9
answered it: `)020000` returned `FB 02`, and `)010000` / `)010001` on the same radio returned `FB`
and `02`. It is a two-byte read, not a serial-number command. **[C]**

**P-23** `)40`+addr — read 64 bytes, reply `(40`+addr + 128 nibble-characters. During a sequential
read every record after the first is requested with a leading `0x06`, the acknowledgement of the
previous one — confirmed on hardware, where all four EZA 9 records after the first carried it.
**[C]**

**P-23a The address space is 10 bits, and overrun aliases instead of failing.** Two limits, both
enforced in firmware and both worth respecting client-side:

* `proto_ReadHeader` NAKs a **count over `0x40`** or an **address high byte over `0x03`** before doing
  anything else — so the reachable window is `0x0000`–`0x03FF`, 4 banks × 256.
* More dangerous: the address that actually reaches the bus is **`addr & 0x03FF`**. The I²C device
  byte is built as `0xA0 | ((addr >> 7) & 6)`, which keeps only address bits 9:8 and **discards
  everything above**. A request past the top therefore does not error — it returns *plausible data
  from bank 0*. The write loop re-checks the range on every byte (`CMPA #$03 / BHI`), but **the read
  loop has no range check at all**, so an unaligned request can walk off the top inside the record.

`mc_read_block` refuses `addr + 64 - 1 > 0x03FF` rather than accept silently wrong bytes. **[S]**

**P-24** Past the end of memory the radio NAKs, in one of **two forms — both real**:

| form | seen on |
|---|---|
| echoed 7-byte header, then `0x15` | the EVA captures |
| a bare `0x15` | an EZA 9, on hardware |

Accepting only one misparses the end of every read on the other. This is not a spec ambiguity to be
resolved but a difference between radios, so accept both. **[C]**

**P-24a The end-of-memory NAK ends the session — on the radios tested.** After it the radio answers
nothing — not the next command, and not even `*`. Measured on every radio put through the selftest so
far: four Radius M110s (two `EZ3.01.00.44` CSQ/PL, 70 cm and 2 m; two `EZ9.01.00.45` Sel 5, 70 cm
and 2 m) and an MC micro EZA 9, across nine runs. **[C]**

The disassembled EVA firmware returns to its command loop after sending the NAK rather than going
deaf, so this may be **model-specific rather than universal**. No EVA has been put through it, so
the in-band recovery path is retained **until one is on the bench**. **[S]** **[?]**

**P-24b A second way the session dies: the Ext Alarm input.** Independently of any NAK, the radio's
character reader checks **pin 15** on every call and abandons programming mode if it is
asserted — `ser_GetChar` at `E7A3` tests Port 2 bit 6, powers the EEPROM down, and jumps out. It picks
the exit by the write-in-progress flag (`$0087` bit 6): `EDDD` if a write was underway, `FA3D`
otherwise. So a radio can go silent **mid-record with no protocol error at all**, and the wire looks
identical to a radio that simply stopped answering.

Practical consequence: when a session dies unexpectedly, a stuck or noisy Ext Alarm line is a
candidate alongside P-24a, and it is one the protocol cannot distinguish. Worth knowing before
attributing every silence to the NAK. **[S]**

Anything that needs the radio after a full read must re-establish the session first. A power-cycle
is the only method **known** to work — but the 1987 software suggests a second one that has never
been tried.

**P-25** `(40`+addr+128 chars — write 64 bytes. The reply is **two bare ACK bytes, no header**: the
first ~130 ms after the last data byte (command accepted), the second **~710 ms** later (EEPROM burn
complete). Measured across all 8 blocks of the write capture, consistent to 3 ms. **[C]**

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

**P-31** **2000 ms** for the write's second reply, measured from completion of transmission. Note
that `tcdrain`/`FlushFileBuffers` on a USB bridge does not reliably mean "on the wire", so matching
the observed 130/710 ms exactly is not achievable — the *ordering* requirement of P-25 is what is
absolute. **[S]**

**P-25a The radio's state after a failed write is itself a measurement.** A write that fails can mean
the record was refused or that the radio left programming mode entirely, and those want different
answers. The selftest asks `*` afterwards, and if that is silent runs the P-27 arming sequence and
asks again, recording which of the three happened: survived, recovered in band, or needs hands on the
radio. **[C]**

**P-25b A frame of write length that is not a write.** Same 135 bytes, same address, same payload —
the radio's own bytes — but a command letter nothing handles. If the session dies on this too then
**length** is what the radio cannot survive; if it survives, the objection is to being told to write.
One frame, and it separates two hypotheses that the write probe alone cannot. **[C]**

**P-31a The burn is a timed loop, and its duration is derivable.** The radio does not poll the EEPROM
for write completion; `eep_WriteByte` waits out a fixed delay per byte —
`LDX #$0BF7 / DEX / BNE` = 3063 × 4 = **12252 cycles**, which at E = 4924800/4 = 1231200 Hz is
**9.95 ms**. With the bit-banged I²C transaction on top it is ≈ **10.89 ms per byte**, so a 64-byte
record predicts a **≈ 697 ms** gap between the two ACKs. **[S]**

**P-31b The two ACKs need different timeouts.** The first is sent when the record has been taken into
RAM, *before any byte reaches the EEPROM* (firmware `E7F6`, before the burn loop at `E7FE`), so it
arrives within a character time — the captures show ~130 ms. Only the second waits out the burn.
`MC_T_ACK1` = 400 ms and `MC_T_BURN` = 2000 ms. Separating them distinguishes **"the radio never took
the record"** from **"it took the record and stopped partway through committing it"**. **[S]**

**P-31c There is no rollback.** The radio burns byte by byte and ACKs only at the end, so a failure
between the two ACKs leaves an **unknown prefix of that record already committed**. Nothing undoes it.
Error text must say so, and a failed write must be followed by a verifying read rather than a blind
retry — this is the same reasoning as P-32's "no retry around write". **[S]**

**P-31d The ACK clock starts when the frame has LEFT, and a driver may not be asked when that was.**
P-31 already said "measured from completion of transmission". The implementation did not do it, and
that divergence cost eight hardware sessions.

`send()` returns when the kernel has *buffered* the frame. A write frame is 135 bytes; at 1200 baud,
8 data bits inside a 10-bit character, that is **1125 ms on the wire**. `MC_T_ACK1` is **400 ms**. So
the window closed while the radio was still receiving **byte 48 of 135**, and no radio could ever
have answered inside it. Every read in the same sessions worked because a read command is 7 bytes —
**58 ms** — and never came close to the limit. That asymmetry is the signature: four radios, eight
sessions, every read fine and every write reporting `no first ACK`, identically. **[C]** **[S]**

The fix is **arithmetic, not `tcdrain`**, which is what P-31 warned about above:

- On a **USB-serial bridge** (FTDI, CH340, CP210x, PL2303) the driver knows its own queue and not
  the adapter's FIFO, so `tcdrain` can return while the frame is still going out beyond the USB
  link — reintroducing this bug invisibly, on exactly the hardware most people now use.

  **Measured on an FTDI FT232 (`0403:6001`), macOS, behind a USB-C hub, at 1200 baud. It is not a
  small error — the kernel is simply blind past the USB boundary. [C]**

  | | 135-byte frame | 512-byte frame |
  |---|---|---|
  | the wire needs | **1125 ms** | **4267 ms** |
  | `write()` returned after | 0.0 ms | 0.1 ms |
  | **`tcdrain()` returned after** | **0.1 ms** | **0.1 ms** |
  | `TIOCOUTQ` reported empty after | — | **506 ms** |
  | `drain()`, computing the floor | **1128 ms** ✓ | — |

  `tcdrain` waited for **nothing at all**, and `TIOCOUTQ` — the other obvious way to ask — calls a
  4.3-second transmission finished after half a second. Had the `tcdrain` version shipped, P-31d
  would have been **entirely unfixed on this adapter**, failing identically and looking like the
  radio again.

The floor is padded rather than exact: **+3 ms and +1 %**. Overshooting is free — a reply arriving
during the wait is buffered by the kernel and reading it late costs nothing — while undershooting
comes straight out of `MC_T_ACK1`, which is the defect itself. The fixed part covers the transfer
crossing the link before the adapter starts shifting; the proportional part covers baud generators
whose divisor is not exact. On the FT232 at 1200 baud the 135-byte frame is charged **1139 ms**
against a physical 1125.
- On a **pty** it was measured **blocking for 2 s**, long enough for the peer in `test_serial.c` to
  hit its idle timeout and exit, so the ACK never came at all. **[C]**

*n* bytes at *b* baud cannot leave in less than *n* × 10 / *b* seconds, whatever any driver claims.
`send()` accumulates that per frame and clamps it forward to the current time; `drain()` sleeps to
it. It cannot block, cannot hang, and cannot be lied to. The speed charged is the one explicitly
requested, else the port's actual speed (so `--baud 0` is not a silent hole), except on a pty, which
has no wire and is charged nothing.

**P-31e The write works on hardware; the M110 burn constant is measured.**
Four radios, two models, `reports/write-runs4`: record accepted, burn confirmed, record read back
**identical**, radio still answering afterwards, full EEPROM read completing in the same session.
Report 1 is **15 probes, 12 as documented, 0 differ, 0 failed**. **[C]**

| radio | burn gap, 64 bytes | per byte |
|---|---|---|
| `EZ3.01.00.44` CSQ/PL | **3939 ms**, 3937 ms | 61.5 ms |
| `EZ9.01.00.45` Sel 5 | **3252 ms**, 3252 ms | 50.8 ms |
| EVA, predicted by P-31a from `EZA33.BIN` | 696 ms | 10.89 ms |

The M110 burn is **five times the EVA's**. `MC_T_BURN` at 2000 ms was short by a factor of two and
could never have worked on this family; at 8000 it leaves 2.03× over the slowest observed. The two
readings per radio differ by 2 ms and 0 ms, so this is a fixed timed loop, as P-31a says it is.

Two `DIFFERS` in that batch are both benign: an unprogrammed radio reporting band index 7, and a
**40-byte ident** on the M110 Sel 5 against the EVA's 41 — ident length is per-model (P-20 already
records 37 on the EZA 9).

**What it took, in order:** P-31d (start the ACK clock when the frame has left, not when it was
queued), then this timeout, then not interrogating a radio that is still burning.

**P-31e (superseded) A radio accepted a record and did not confirm the burn.** `reports/write-runs3/report1`, an `EZ3.01.00.44` M110. The wire log settles P-31d beyond
argument:

| | |
|---|---|
| write frame queued | `t = 6691 ms` |
| **first ACK (`06`) received** | **`t = 7831 ms`** |
| gap | **1140 ms** |

1140 ms is the 1125 ms the frame occupies at 1200 baud plus the radio's turnaround. The old 400 ms
window could not have seen it, and every earlier run reported `no first ACK` for that reason alone.
**[C]**

What failed instead is the **second** ACK. `MC_T_BURN` was 2000 ms — ~3× the 697 ms P-31a derives —
and nothing arrived in 2001 ms. But 697 ms is an **EVA** figure, read out of `EZA33.BIN`'s timed
loop; the M110 runs `EZ3.01.00.44`, whose burn constant nobody has read. 2000 ms allows only **31 ms
per byte** and was never measured for this radio. It is now **8000 ms** — 125 ms per byte, past any
plausible EEPROM — because a receive timeout costs nothing when the radio answers.

The radio then went silent for the rest of the session and `P-24a`'s arming pulse did not revive it;
it needed a power cycle. **What is not established** is whether the six `*` probes the selftest sent
immediately afterwards contributed to that, or merely followed it. There is no reason to poke a
radio that has just said it is writing, so the selftest now settles for a full burn timeout before
asking anything. **[?]**

**P-32** Exactly **one** retry, of the attention phase only. **No retry** around read, write or
verify — fail loudly. An automatic retry mid-write is how a glitch becomes a half-written EEPROM.
**[S]**

## 5. Sessions

**P-40 Identify.** The opening exchange in both read captures is `)01 0000` → `*` → `)01 0000`:
probe, identify, probe. A write session opens with the probe alone. **[C]**

**P-41 Read all.** For `addr = 0, 0x40, 0x80, …`: send `)40`+addr; on a valid header read 128 payload
characters and ACK with `0x06`; on NAK (either form, P-24) stop. Device size = records × 64. Cap the
walk at 64 records. Verify the checksum (K-2) and **report** a failure without refusing the data.

**P-42 Write all.** Per record: `(40`+addr+payload → wait ACK1 → **wait ACK2** → `)40`+addr read-back
→ compare. Comparison is byte-exact **except** channel frequency fields, which compare by **decoded
frequency** (K-11). Any real mismatch aborts immediately, naming record and offset.

## 6. Codeplug

**K-1 Image.** The codeplug is the device's EEPROM verbatim. `.DAT` files are exactly these bytes,
so old files load unchanged. **[C]**

**K-2 Checksum.** One byte is chosen so that the covered range sums to a per-model **target** mod
256. **The byte, the extent and the target are all per-model** (K-20). On every MC micro model the
target is `0xFF` and the extent is the whole device, MCEZ13 included; the sum loop at `CS:0x767E`
runs `0 .. size-1` and `size` is 128 there — watched at runtime, not inferred. **[C]**

**K-10 Frequency.** Each 3-byte field:

```
coarse = ((b0 & 3) << 8) | b1
step   = 3125 Hz if (b0 & 4) else 2500 Hz
freq   = (coarse * P + b2) * step
```

The RX field holds the local oscillator; the displayed RX frequency is **field + 21.4 MHz** (the
first IF). `P` = 80, 80, 128, 254 for bands 1–4. Band index 7 means unprogrammed — ask the user,
do not error. **[C]**

**K-10a The reference dividers are checkable.** `REF_DIV.001`, shipped beside the RSS on every disk
set, is the operator's reference card, and its four values decode exactly as
**`word = 2 × (Fref ÷ spacing) + 1`**:

| Fref | 5 kHz | 6.25 kHz |
|---|---|---|
| 14.4 MHz (standard) | `0x1681` | `0x1201` |
| 12.8 MHz (SP) | `0x1401` | `0x1001` |

So `refdiv[0]` is the **5 kHz** divider and `refdiv[1]` the **6.25 kHz** one — they are not
interchangeable. The band byte's raster bit therefore has **a second job**: besides the channel
raster it selects which of the pair is in force. All eleven EVA sample codeplugs carry the standard pair. `mc_refdiv_spacing()`
returns the implied spacing for a word, or 0 if it is not one of the four.

This does **not** license computing them — K-30 still says preserve verbatim, because a radio may
carry an SP crystal or a value nobody has seen. It licenses *reporting*: a pair that is neither
standard nor SP, or one implying two different `Fref`, is recognisably wrong rather than merely
unfamiliar. **[C]**

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
radio). The **count** byte's low nibble is a selectable-lockout marker and must be preserved (K-30).
Range is 67.0–250.3 Hz, or 0 to disable; anything else is refused, never rounded (U-3). **[C]**

**K-30a The mode byte's low nibble is the radio's index into the tone list.** It is not unknown
padding, and preserving it across a mode change is a bug. The EVA firmware fetches the tone as
`cp_pl_list[mode & 0x0F]` at `0x047 + 2i` — `F270`: `ANDB #$0F / ASLB / LDX #$47 / ABX` — gated on
**bit 6** of the same byte, which is why both `0x60` and `0xE0` carry it. **[S]**

The consequence is concrete. In single-tone mode the tone is written to slot 0 (`pl_tone` ==
`pl_list`), so a stale nibble makes the radio read a **different slot than the programmer wrote**:
set single-tone on a codeplug holding `0xE7` and a masking implementation leaves `0x67`, so the tone
goes to `0x047` while the radio reads `0x047 + 14 = 0x055`.

The 1987 RSS does not have this problem — it **assigns** `0x1FD := 0x60` rather than masking, and the
differential sweep caught it turning `0xE7` into `0x60`, clearing the nibble. **[C]** So:

| mode | mode byte |
|---|---|
| single | `0x60` exactly — index 0, the slot the tone was written to |
| selectable | `0xE0 \| index`, the operator's own choice, **clamped** below the populated count |
| off | `0x00` |

**K-31 The trakmode block is derived from the codeplug size, not hardcoded.** A *trakmode* is
Motorola's term for a shared signalling personality — one **27-byte** record holding the basecall
code, repeater-access sequence, groupcall positions, PL encode mode, AAK/secret bits and the
signalling FORMAT. Records are counted **downward from the top of the codeplug** and every channel
points at one via record byte `+1`. The EZA family has no such block. **[C]**/**[S]**

The radio does not take the codeplug size on trust. It reads **bit 7 of `0x0CF`** — clear = 512
bytes, set = 1024 — and derives everything from that:

```
top        = (cp[0x0CF] & 0x80) ? 1024 : 512      EZA33 E969 (checksum), F512 (trakmode)
TrakBase(t)= top - 27*(t+1)
PL mode    = TrakBase(t) + 0x18
```

For a 512-byte EVA that gives `TrakBase(0) = 0x1E5` and a PL mode byte at **`0x1FD`** — which is what
MCprog previously hardcoded. For a 1024-byte codeplug it moves to **`0x3FD`**, and the hardcoded value
would have edited the wrong byte. `mc_pl_mode_off()` is now authoritative and `pl_mode` is its
512-byte fallback; the write gate asks the same function, so it cannot guard one address while the
editor writes another. **[S]**

Accessors: `mc_codeplug_top()`, `mc_trak_base()`, `mc_trak_count()` (bits 0–3 of `0x0DA`),
`mc_trak_enabled()` (bit 7), `mc_channel_trak()`. **PL uses trakmode 0 deliberately** — a 512-byte
EVA has room for exactly one record and every sample codeplug carries trakmode `0x00` on every
channel, so "per-trakmode" and "radio-wide" coincide. Resolving a channel's own trakmode needs a
codeplug that actually has more than one, and none exists to test against. **[?]**

**K-14a PL in the channel record, and the two laws.** The Radius M110 CSQ/PL gives **every channel
its own** encode and decode tone, in the record rather than in a shared table: **encode at `+0`,
decode at `+5`**, straddling the TX triplet. Both are big-endian words, 0 meaning none. **[C]**

The two fields use **different scales**, which is why one tone stores as two different numbers:

| field | law | 123.0 Hz stores |
|---|---|---|
| encode (`PLE`) | `round(f × 7.9844)` | **982** = `0x03D6` |
| decode (`PLD`) | `round(f × 61.107)` | **7516** = `0x1D5C` |

Neither constant is new. `7.9844` is K-12's MCEZ13 scale and `61.107` is the decoder law MCEZ13
already uses for its `0x010` table — so **the two constants are not per-model at all: the encoder
and the decoder simply scale differently**, and MCEZ13 and the M110 are the two radios that expose
both. Measurement on MCEZ13 had only bounded the decoder constant to `[61.1063, 61.1087]`.

**K-15 Auto-acknowledge delay.** One byte, `round(ms / 15.625)` — a count of 1/64 second — range
1–127, i.e. 16–1984 ms. Radio-wide. Only the EZA 9 map has it, at `0x076`. Bit 7 is never set by
the original software and its meaning is unknown, so it is preserved (K-30) and the value is the
low seven bits. Values outside 16–1984 ms are refused, never clamped (U-3). **[C]**

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

**K-20 Model descriptor.** These genuinely vary and must be data, not assumptions.

The three-byte family tag at `0x07..0x09` identifies the *family*, not the model: `EZ3.` and `EZ9.`
are **shared between the MC micro EZA radios and the Radius M110s**, separated only by the version
that follows. Detection keys on size, checksum and marker; the prefix alone cannot do it.

| model | size | checksum byte / extent | channels | band | ref dividers |
|---|---|---|---|---|---|
| `MCEV_56` 5/6-tone | 512 | 0x000 | 0x0E0, 32 × 8 | 0x0DC b4-6 | 0x0D4 |

| `MCEV9` / `MCEV9M` SEL5 EVA | 512 | 0x000 | 0x0E0, 32 × 8 | 0x0DC b4-6 | 0x0D4 |
| `MCEZ9` SEL5 EZA | 256 | 0x000 | 0x0C8, 8 × 6 | 0x082 b4-6 | 0x0C4 |
| `MCEZ13` CS/PL | 128/256/512/1024 | **0x003**, whole device | 0x03B, 8 × 6 | 0x039 b4-6 | 0x004 |
| **M110** CSQ/PL `EZ3.01.00.44` | 256 device / **128 codeplug** | **0x00F**, first 128, target **0x01** | 0x01B, 10 × 10 | **0x00A b0-3** | 0x013 |
| **M110** Sel 5 `EZ9.01.00.45` | 256 | **0x00F**, whole device, target **0x01** | 0x092, 15 × 7 | **0x00A b0-3** | 0x089 |

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
`N`. The EVA row is pinned on the SEL5 build **and independently reproduced on the
5/6-tone one**: driving `MCEV_56`'s editor and diffing what the radio received moves bit 3 for
clock shift, 4 for decode, 5 for TX inhibit, 6 for encode and 7 for RF power — every one in **both
halves** of the record, and only in the channel addressed. It also shows that the two builds differ in
*which half they read*: `MCEV9M` writes clock shift into bit 3 of both halves, `MCEV_56` displays
the TX half's bit 3 alone. Write both halves and the difference cannot bite you
(`../doc/EEPROM_MAP.md`). **[C]** MCEZ13 has no per-channel encode/decode/TX-inhibit at all; its PL lives in tables and TX
inhibit is a single global bit. **[C]** except as marked.

**K-23 The channel table is terminated, not sparse.** Stop at the first record whose `+0` is `0xFF`.
Records past the terminator are stale and **must be written back unchanged**. **[C]**

**K-24 Three channel states.** Beyond the terminator (does not exist), allocated but unprogrammed
(valid number, zero frequency), and programmed. Never render an unprogrammed slot as `0.00000 MHz`.
**[C]**

**K-25 The device size, the codeplug size and the write extent are three different numbers.** On
every MC micro model they coincide. On the **M110 CSQ/PL** they do not: the device returns 256 bytes
that are **two identical 128-byte copies** (`bytes[:128] == bytes[128:]` on both hardware radios),
the codeplug is the first 128, and the 1989 programmer writes **only those 128** — its emitted
record list is `[(0,64), (64,64)]`, against `[(0,64), (64,64), (128,64), (192,64)]` for the Sel 5.
So the write extent is per-model. **[C]**

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

**U-5 Storno CQM 5500 is the same hardware, and must not become a model.** Storno resold these
radios rebadged, and its programmer is the Motorola one **relocated, not rewritten** — same wire
protocol, same codeplug layout, same checksum. Driven side by side under emulation, `CQMEZ13` and
`MCEZ13` put **byte-identical** traffic on the wire (31 bytes, same event sequence), as do `CQMEZ9`
and `MCEZ9` (54 bytes) and `CQMEV9` and `MCEV9` (86 bytes). Every Storno build's codeplug buffer
sits exactly `+0x22` above its Motorola sibling's, which is a difference in the *programmer's* data
segment and nothing to do with the radio. **[C]**

So the model table carries Storno's own name for each radio (`.storno`, taken verbatim from
`STORNO.COM`'s table) and `--model` accepts it, including any unique fragment; an ambiguous fragment
resolves to nothing rather than to a guess. **Adding a `storno` model would be inventing a
distinction nothing supports.**

**U-6 Record the ident the original software demands; never enforce it.** Each model carries
`.rss_ident`, the prefix its 1987/89 software requires — `EV9.00.`, `EZ9.00.`, `EZ3.00.` for the MC
micro families, the full `EZ9.01.00.45` / `EZ3.01.00.44` for the two M110 **variants**
(four radios, two idents). It is informational.
MCprog must not refuse a radio on it, because the original software's rule is demonstrably too
narrow: a **real EVA answers `EV9.01.00.11`**, and the 1987 **Standard** build rejects it with
`ERROR : INVALID TYPE` while the **Master** and **Repair** builds of the same version read it
without complaint. Sweeping the ident one field at a time shows only the version decides — the model
code and the description change nothing. **[C]**

**U-7 A codeplug can be created without a radio, but only from a genuine factory default.**
`mcprog --new MODEL [--band N] FILE` writes a new image; `--list-defaults` shows what is available.
Everything else about file mode already works with no radio attached — `mcprog FILE` edits,
`--dump-vec FILE` decodes.

**The images are captures, not constructions, and that is the whole point.** A codeplug holds many
bytes this project has never mapped; an image built from only the fields MCprog understands would be
wrong in ways nothing here could detect, and the radio would take it. So `--new` does what the
repair build's `INITIALIZE - 4 (reset to default)` does — it starts from a real factory default —
and those defaults were captured off the wire from that build (`tools/eza.py default_codeplug()`,
`doc/EEPROM_MAP_EZA.md`). What `--new` writes is byte-identical to what the 1987 software produced.
**[C]**

Nine are compiled in: `eza_sel5` and `eza_cspl` for RF ranges 1–4, and `eva_sel5`, whose build has
no RF-range prompt at all so its band byte is left unset. **Neither M110 has one**, because none has
been captured — and `--new m110_sel5` says so and lists the alternatives rather than inventing an
image or reusing some real radio's, which would embed that radio's serial number.

Three safety properties, each tested: the built-in default is **checked against its own model's
size and checksum before being written**, so a corrupted capture cannot reach a file; an existing
file is **never overwritten without `--force`**; and `--new` **refuses to be combined with `--port`,
`--read`, `--write` or `--selftest`**, because creating a file and touching a radio are different
operations and merging them is how the wrong image gets programmed.

## 8. Write safety

**W-1** Writing requires an explicit opt-in flag; absent it the action is visible but disabled, with
the reason shown.
**W-2** A full read of the radio is dumped to a backup file before the first write byte. Failure to
read or to write the backup aborts the write.

**W-3** Pre-write gates, all fatal: checksum valid; band not 7; model and size match; every byte
that differs from what the radio just returned is one MCprog itself writes (K-30). A write that
would change nothing is refused rather than performed.

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

**W-6** Record order 0..N faithful to the original. `--checksum-last` may be offered, but the
faithful order is the only one known to work on hardware.

