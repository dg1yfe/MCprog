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
explicitly. **[S]** — confirmed from the code: `ser_OpenLine` writes only `MCR=0x00` and `MCR=0x02`,
so DTR (bit 0) is never asserted by the original at all.

**P-12** `MCR=0`, wait 500 ms, assert RTS, wait 1300 ms — **before every transaction**, not once on
open. **[S]**

**P-27 The original runs that sequence TWICE, back to back, at the start of an operation — and not
again within it. MCprog now does the same. [C]**

> Measured rather than read off the prose, because "before every transaction" is easy to misread as
> recurring mid-session. Driving `MCEZ9` under emulation with the radio's control lines
> instrumented, a whole **read** — probe, identify, probe, four record reads, the end-of-memory NAK
> — produces exactly four line transitions, and every one is at **TX byte zero**:
>
> ```
> dtr=0 rts=0   at TX byte 0     \  ser_OpenLine
> dtr=0 rts=1   at TX byte 0     /
> dtr=0 rts=0   at TX byte 0     \  and again
> dtr=0 rts=1   at TX byte 0     /
> ```
>
> A **write** session — 4 records, 575 bytes — gives the same four and no more. So the boundary is
> the *operation*, not the command and not the record.

`mc_session_arm()` reproduces it, and `main.c` calls it at the head of the read and the write. The
**pulse on `mc_serial_open()` was removed**: the 1987 software has no persistent open, and leaving
it in would have made three pulses where the original makes two.

Two consequences worth stating, because both are easy to get wrong:

* **`--no-line-setup` gates the automatic arming, not an explicit request.** The transport's `rearm`
  hook is left NULL when the flag is off, so `mc_session_arm()` does nothing — but a direct
  `mc_serial_rearm()` still pulses. The selftest depends on that: it sets `line_setup = 0` because
  it drives the lines itself, and `P-24a` must still be able to pulse deliberately.
* **Arming clears `pending_ack`.** The rising edge restarts the radio's programming routine, so no
  acknowledgement can be owed across it.

Why *twice* is **[?]**. It is reproduced because the object is to be identical, and a second NMI
costs nothing but time.

> **Read out of the 1987 code, 24 Aug 2026 [S].** The routine is `ser_OpenLine`, at file offset
> `0x6E6B` in `merged/MCEZ13M_rt.bin` (IP = offset + 0x100). Its port sequence is **byte-identical
> in all 22 images that carry it**, the 1989 repair build included. In the EZA and `CQM*` images it
> sits in the merged binary; in the EVA / EV56 / centro family it lives only in the **overlay**
> (`M5/MCEV_56.001` +`0x1EDB`, `M4/MCCENTRM.000` +`0x32E5`, `M5/MCEV9M.000` +`0x6C9`), which is why
> a static scan of the merged images finds nothing at all. In order:
>
> ```
> LCR = 0x80          DLAB on
> DLH = 0x00 ; DLL = 0x60      divisor 96 = 1200 baud
> LCR = 0x0A          7 data bits, odd parity, 1 stop  (built as `mov ax,8 / add ax,2`)
> IER = 0x00          no interrupts, polled
> MCR = 0x00          DTR and RTS both DOWN
>     wait 0x32 = 50
> MCR = 0x02          RTS UP, DTR still down
>     wait 0x82 = 130
> ```
>
> The delay unit is a **centisecond**, which is what makes the two constants 500 ms and 1300 ms:
> the clock helper (`0x6D8D`) calls `INT 21h AH=2Ch` and returns `DH*100 + (DL/10)*10` — seconds
> and hundredths within the current minute — and the deadline helper (`0x6DD5`) wraps it modulo
> **6000** (`cmp 0x1766` / `sub 0x1770`), i.e. 60 s at 100 Hz. The expiry test (`0x6E19`) is
> `(now - deadline) >= 0 && < 100`, the upper bound guarding that same wrap. Resolution is 100 ms,
> so the real waits are 500 and 1300 ms ±100 ms. P-12's timings were previously marked **[S]**
> without this chain; they are now derived from the constants themselves.
>
> `LCR = 0x0A` is an independent confirmation of **P-2**: the original drives the UART at 7O1 in
> hardware, exactly the frame MCprog synthesises in software on an 8N1 port.
>
> Only the sequence itself is common. The clock helpers around it are **not**: MCEZ9R (1989)
> rewrote them to return `DH*100 + DL` with a two-minute signed span instead of truncating to
> tenths, so its resolution is 10 ms rather than 100 ms. The unit stays a centisecond, so `0x32`
> and `0x82` still mean 500 and 1300 ms there. That a repair build shipped a 10× finer clock is
> worth remembering against the fast-PC problem.
>
> **The RSS never reuses an open link.** Driving each build under emulation, the whole sequence
> runs once per radio operation: one bounce per read, and one before a write (the EV56 write emits
> 8 record-writes after a single bounce, so it is per *operation*, not per record). Confirmed
> independently from the call graph rather than the wire: `ser_OpenLine` is invoked by
> `proto_AttentionBody`, which both the read driver and the write driver call. MCprog did it
> **only on open** until `mc_serial_rearm()`. See the note under P-24a.

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
> user, who has built the interface. More precisely, and from the same source: that line reaches
> the **`#NMI` input of the radio's CPU**, and taking RTS *high* issues the NMI that (re-)starts
> programming mode. **[user]** So RTS is an edge-triggered *command*, not a level the radio sits
> in, which is exactly why P-12 pulses it rather than merely holding it -- and why the original can
> re-enter programming mode at will. Consistent with the schematic: DTR drives a BC557 (a PNP,
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

> This also explains **where the end-of-memory NAK comes from**. The firmware is willing to address
> 1024 bytes, but the fitted part is smaller — 128 on an EZA CSQ/PL, 256 on an EZA Sel5 and both
> M110s, 512 on an EVA. Reading into a bank that is not populated gets **no ACK from any device**,
> which the firmware reports as the NAK of P-24. "End of memory" is the bus going unanswered, not a
> firmware limit being reached. **[S]**

**P-24** Past the end of memory the radio NAKs, in one of **two forms — both real**:

| form | seen on |
|---|---|
| echoed 7-byte header, then `0x15` | the EVA captures |
| a bare `0x15` | an EZA 9, on hardware (17 Aug 2026) |

Accepting only one misparses the end of every read on the other. This is not a spec ambiguity to be
resolved but a difference between radios, so accept both. **[C]**

> **Where the first form comes from [S], 24 Aug 2026.** The EVA radio firmware (`EPROM/EZA33.BIN`,
> `doc/EZA33_FIRMWARE.md` §5a) echoes the parsed header — `(`, count, both address bytes — at
> `E763`–`E771`, *before* it reads the first EEPROM byte. A NAK raised later, in the byte loop, is
> therefore preceded by the complete 7-character echo; a NAK raised in the header parser
> (`proto_ReadHeader`, `E7CF`, which rejects a count over `0x40` or an address high byte over `0x03`)
> comes bare. That accounts for the EVA row exactly. It does **not** account for the bare `0x15` from
> the EZA 9, whose addresses were in range — different firmware, not disassembled. **[?]**

**P-24a The end-of-memory NAK ends the session — on the radios tested.** After it the radio answers
nothing — not the next command, and not even `*`. Measured on every radio put through the selftest so
far: four Radius M110s (two `EZ3.01.00.44` CSQ/PL, 70 cm and 2 m; two `EZ9.01.00.45` Sel 5, 70 cm
and 2 m) and an MC micro EZA 9, across nine runs. **[C]**

> **An EVA firmware does not behave this way [S], 24 Aug 2026.** In `EPROM/EZA33.BIN` the NAK path is
> `LDAA #$15 / BSR ser_PutChar / BRA $E787`, and `E787` is `BRA $E74E` — the top of
> `nmi_ProgramMode`'s command loop. It sends the `0x15` and goes straight back to waiting for the
> next sigil, with the stack reset. Both NAK sources behave this way: a rejected header and an
> EEPROM that fails to ACK.
>
> **None of the three radios measured above is an EVA**, so this is not a contradiction — it is
> evidence that P-24a is **model-specific rather than universal**. The `[C]` measurements stand for
> the radios they were taken on. Nothing here should be assumed for an EVA until one is on the bench,
> and nothing in the client may drop the recovery path on the strength of one disassembly. **[?]**

> **This is why nothing has been written to a radio yet.** Four write-enabled runs on 23 Aug 2026
> all reported `write 0x0000: no first ACK`. The wire log shows the write frame was well formed —
> exactly the 135 bytes P-25 specifies, `(40` + address + 128 characters — and that **no bytes came
> back at all**. The read-only runs settle the cause: they lose the radio at the same point having
> written nothing, so it is the NAK and not the write.
>
> The selftest sent the write *after* walking the whole EEPROM, so the radio had already gone quiet.
> It now takes one record on its own first (`mc_read_block(..., chain = 0)`), exercises the write
> path with that, and walks the EEPROM afterwards. **Untested on hardware** — the next radio through
> is what settles whether the write path works at all.
>
> **And that same run now tests the recovery.** The selftest's last act is a `*` after the full
> session, which on every radio so far has gone unanswered. When it does, the selftest pulses RTS
> (`mc_serial_rearm()`) and asks again, reporting `P-24a` as PASS if the radio comes back. It runs
> last, on a radio that is already unresponsive and after the codeplug has been read and saved, so
> failure costs nothing and success retires the power-cycle requirement outright. It is skipped when
> the ident answered — there would be nothing to recover — and on a transport with no control lines.

**P-24b A second way the session dies: the Ext Alarm input.** Independently of any NAK, the radio's
character reader checks **pin 15** on every call and abandons programming mode if it is
asserted — `ser_GetChar` at `E7A3` tests Port 2 bit 6, powers the EEPROM down, and jumps out. It picks
the exit by the write-in-progress flag (`$0087` bit 6): `EDDD` if a write was underway, `FA3D`
otherwise. So a radio can go silent **mid-record with no protocol error at all**, and the wire looks
identical to a radio that simply stopped answering.

> **What pin 15 is, corrected 27 Aug 2026.** It is the **timer-2 output** — the alert tone in normal
> operation — and an input only while programming mode has stopped that timer. The data-direction
> register says nothing about it either way, because a timer output pin overrides the DDR. The ROM
> hands it over explicitly on entry: `E736 CLRA / E737 STAA $1B` stops timer 2 and `E739
> AIM #$BF,$01` makes P2.6 an input, adjacent instructions with nothing between them. See
> `doc/EZA33_FIRMWARE.md` §8. **[user, schematic]** + **[S]**

Practical consequence: when a session dies unexpectedly, a stuck or noisy Ext Alarm line is a
candidate alongside P-24a, and it is one the protocol cannot distinguish. Worth knowing before
attributing every silence to the NAK. **[S]**

Anything that needs the radio after a full read must re-establish the session first. A power-cycle
is the only method **known** to work — but the 1987 software suggests a second one that has never
been tried.

> **What the original does instead of a power-cycle [S], 24 Aug 2026.** It re-establishes the
> session *by construction*: it runs the whole P-12 sequence — full 8250 re-init, `MCR=0` for
> 500 ms, RTS back up, 1300 ms — **before every single transaction**. It therefore never faces the
> question P-24a poses, because it never carries a session across a read boundary. Under emulation
> the second read of a two-read session is preceded by exactly this bounce, at the instruction the
> first read's last byte leaves off.
>
> **And there is a mechanism for it. [user]** RTS reaches the radio CPU's **`#NMI` input** through
> the interface circuitry, and taking RTS **high** issues that NMI — which is what (re-)starts
> programming mode. So the `MCR=0` → `MCR=2` bounce is not a handshake and not line housekeeping:
> it is the RSS **deliberately firing an NMI at the radio** to put it back into the programming
> routine, and the 500 ms low is just long enough to guarantee a clean edge. That is why the
> original can afford to ignore P-24a entirely — it re-enters programming mode from scratch before
> every transaction, so a session that ended at the NAK never needs to survive.
>
> This makes the RTS pulse the **first thing to try** after the end-of-memory NAK, in place of the
> power-cycle: on the still-open port, drop RTS, wait 500 ms, raise it, wait 1300 ms, then ask for
> `*`. A power-cycle and an NMI both land the CPU back at the same entry point, which would explain
> why the power-cycle works.
>
> **`mc_serial_rearm()`** implements exactly this pulse on an already-open port, for both the POSIX
> and Win32 transports. It is not wired into the read or write path: the call belongs wherever the
> session has to survive, and on current evidence that decision needs a radio, not a guess.
>
> The emulator can now at least ask whether the *RSS* is consistent with the account.
> `radiosim.Radio(nmi=True)` makes the simulated radio deaf until it sees a rising edge on RTS and
> deaf again at the end-of-memory NAK; `tools/nmitest.py` drives every build against it. All four
> survive unchanged — same commands served as with the rule off, each arming the radio itself
> (1–4 times, matching the transaction count) — while the negative control, a radio that never
> hears the NMI, is served **nothing at all**. So the rule bites and the RSS satisfies it unaided.
> That is corroboration, not proof: the model is built from the account it tests, and only a radio
> can confirm that RTS reaches `#NMI`.
>
> What remains **[?]**: the P-12 hardware note still reports a radio going
> permanently deaf after RTS was de-asserted, in a run that *did* re-assert it. That is not the
> same experiment — the probe left RTS **down across two whole line-combinations**, several seconds
> including a port close and reopen, and one of those combinations asserted DTR, which by §3 is the
> level shifter's negative rail; with the rail collapsed the edge may never have reached the CPU at
> all. The RSS, by contrast, drops RTS for exactly 500 ms and never asserts DTR (its only two `MCR`
> values are `0x00` and `0x02`, which is also P-11 confirmed from the code).

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

> **This independently confirms the capture.** P-25 measured ~710 ms on hardware **[C]**; the
> disassembly predicts 697 ms **[S]** — **1.9 % apart**, from two completely unrelated methods. That
> agreement also validates the cycle model behind it (the 1-cycle `DEX`, the 4924800/4 clock, and the
> `0x0BF7` constant), which is why the tone-decoder timings in `doc/EZA33_FIRMWARE.md` can be trusted.
>
> It also shows the **old 800 ms timeout was too tight** — 15 % over the derived figure, 13 % over the
> measured one, for a path that has never once succeeded on hardware. A spurious timeout there is
> expensive to diagnose; a generous one costs only the time to notice a genuinely dead radio. Hence
> 2000 ms, and a separate short timeout for the first ACK.

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
- On a **pty** it was measured **blocking for 2 s**, long enough for the peer in `test_serial.c` to
  hit its idle timeout and exit, so the ACK never came at all. **[C]**

*n* bytes at *b* baud cannot leave in less than *n* × 10 / *b* seconds, whatever any driver claims.
`send()` accumulates that per frame and clamps it forward to the current time; `drain()` sleeps to
it. It cannot block, cannot hang, and cannot be lied to. The speed charged is the one explicitly
requested, else the port's actual speed (so `--baud 0` is not a silent hole), except on a pty, which
has no wire and is charged nothing.

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

**K-2 Checksum.** One byte is chosen so that the covered range sums to a per-model **target** mod
256. **The byte, the extent and the target are all per-model** (K-20). On every MC micro model the
target is `0xFF` and the extent is the whole device, MCEZ13 included; the sum loop at `CS:0x767E`
runs `0 .. size-1` and `size` is 128 there — watched at runtime, not inferred. **[C]**

> **The Radius M110 uses a different constant**: its covered bytes sum to **`0x01`**, with the byte
> at **`0x0F`**. Read out of `M110/MRAR0200.EXE`: the writer at file `0x1BD82` zeroes the cell, sums,
> then computes `1 - sum`; the verifier at `0x1BBF6` does `sum / dec / je`. Measured on four radios,
> and the negative controls are decisive — an image doctored to sum to `0xFF`, the MC micro's own
> constant, is **rejected by the M110's own software**. **[C][S]**
>
> Assuming a single global target is therefore not a simplification but a corruption: applying the
> MC micro rule to an M110 rewrites `0x000`, which on that format is **serial-number byte 0**.

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

> **`b0` bit 2 does a second job: it also picks the reference divider.** The radio at `EZA33` `F9C3`
> does `TIM #$04,0,X` on the channel record's byte 0 and loads the PLL's reference divider from
> `0x0D6` when set, `0x0D4` when clear — the same bit that selects the 3125/2500 Hz step. And 3125 =
> 6.25 kHz ÷ 2, 2500 = 5 kHz ÷ 2, so **the step is exactly half the channel spacing** and one bit
> selects both. A programmer that changed the step without regard for the divider would be
> inconsistent with the radio. MCprog does not write either, so this is a reading, not a bug. **[S]**

**K-10a The reference dividers are checkable.** `REF_DIV.001`, shipped beside the RSS on every disk
set, is the operator's reference card, and its four values decode exactly as
**`word = 2 × (Fref ÷ spacing) + 1`**:

| Fref | 5 kHz | 6.25 kHz |
|---|---|---|
| 14.4 MHz (standard) | `0x1681` | `0x1201` |
| 12.8 MHz (SP) | `0x1401` | `0x1001` |

So `refdiv[0]` is the **5 kHz** divider and `refdiv[1]` the **6.25 kHz** one — they are not
interchangeable. All eleven EVA sample codeplugs carry the standard pair. `mc_refdiv_spacing()`
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

> **How this was found.** The radio-side read path (`doc/EZA33_FIRMWARE.md` §7f) traced the full PL
> chain — channel → trakmode → trakmode byte `0x18` → `cp_pl_list` — and that byte is `0x1FD` on a
> 512-byte EVA. The RSS-derived map had already named `0x1FD` "PL encode mode" and recorded the
> `0xE7 → 0x60` transition; what it could not say was *why* the nibble mattered. The firmware says
> why. The regression test plants `0xE7` and requires `0x60`, and it fails against the old masking
> implementation.

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

> The 1987 RSS computes the same thing: `TrakBase(n) = [0x810] - 27*(n+1) + 1` at `0x3A78`, which with
> its stored top index `0x1FF` also gives `0x1E5`. Two implementations, one formula. **[C]**
>
> **No model is 1024 bytes today**, so this is a latent correctness fix rather than an observed bug.
> It is worth making anyway: it removes an assumption we now know the radio does not share, and the
> accessor refuses a block that would fall outside the image instead of running off the end.

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

> The 1989 M110 RSS settles it exactly, carrying both as IEEE doubles side by side with `0.5` and
> `10` — round-half-up on deci-Hz — at `M110/MRAR0200.EXE` file `0x3174D`, and again at `0x315E8`
> and `0x3176C`. `8.208`, the K-16 duration constant, sits in the same block at `0x31E13`. **[S]**
>
> Confirmed on hardware: both CSQ/PL radios carry `982` and `7516` on channel 2, which the RSS
> prints as `,PLE 123.0` and `,PLD 123.0`, and channel 1 is zero in both fields (`N`).
>
> And confirmed against the original software across the whole range: `tools/plsweep.py` hands the
> 1989 RSS each tone in a `.CP`, lets it program a simulated radio, and reads back what it stored —
> **19 encode and 19 decode tones, every one matching these two laws exactly**, with `N` storing 0.
> The measured table is kept in `tools/verify_docs.py`, deliberately not generated from the laws it
> checks. **[C]**
>
> **118.8 Hz is the only tone that separates `7.9844` from `7.9840`** (949 against 948), so it is
> the single point of evidence that the M110 uses the MCEZ13 constant rather than the EVA one — the
> same tone that already distinguishes the two MC micro scales. The sweep measured it: the RSS
> stored **949**. **[C]**
>
> The Sel 5 M110 has no PL at all. Note `+0` is a **real** offset here, so a model with no
> per-channel PL must say so explicitly rather than leave the field zero.

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
| **M110** CSQ/PL `EZ3.01.00.44` | 256 device / **128 codeplug** | **0x00F**, first 128, target **0x01** | 0x01B, 10 × 10 | **0x00A b0-3** | 0x013 |
| **M110** Sel 5 `EZ9.01.00.45` | 256 | **0x00F**, whole device, target **0x01** | 0x092, 15 × 7 | **0x00A b0-3** | 0x089 |

> The M110 is a different radio that answers the same wire protocol, and **no MC micro offset lands
> on a real field**: `eza_sel5`'s reference dividers read `00 00 00 00` and its write counter at
> `0x09E` is live channel data on both families — channel 1's TX middle byte on the CSQ/PL, channel
> 2's RX middle byte on the Sel 5. It is two models, not a variant. **[C]**
>
> Detection therefore needs a **positive marker**, not just size and checksum. Bytes `0x07..0x09`
> hold the family tag `"EZA"` or `"EZ9"` — a documented field, from the RSS's own radio-type
> descriptor table at `MRAR0200.EXE` file `0x31798`, which is also where the checksum offset `0x0F`
> is confirmed statically for both families. **[S]**
>
> The band is **four** bits, not three: `doc/M110_MNEMONICS.md` recorded bits 0-2, but its samples
> were all `VLO`(4) and `VHI`(7), which have bit 3 clear. `ULO`(12) and `UHI`(15) need the fourth.
> Measured P: `VHI` → 80, `ULO` → 254, `UHI` → 254. `VLO`, `MIB`, `UX1`, `UX2` are **[?]** and the
> table holds zero for them, which reads as "not computable" rather than as a wrong answer.
>
> **Unestablished, hence absent from both models:** the PL encoding (the CSQ/PL record carries PLE
> at `+0` and PLD at `+5`, but the tone scale has not been measured), the channel flag bits, and the
> timer block. The channel *counts* are the size of the region the table occupies, which is
> inference from four radios that leave the rest of it zero — see K-25. **[?]**

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

> This is also the whole of the "sums to `0x02`" puzzle: each 128-byte copy independently sums to
> `0x01`, so the 256-byte total is `0x02`. Never an offset error, never a damaged codeplug.
>
> Writing all 256 to a CSQ/PL would be actively harmful. The write-counter bump guarantees the two
> halves differ, and whether the upper half is separate storage or an address alias of the lower is
> **[?]** — both readings are bad. Separate storage leaves two disagreeing copies with no way to
> know which the firmware reads; an alias means record order makes **the second half win**, so the
> radio ends up holding the copy that was *not* checked.
>
> Channel *counts* on both M110 models are the size of the region the table occupies, which is
> inference from four radios that leave the rest of it zero, not a measurement. **[?]**

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

> The family prefix alone cannot identify a model, and that is why it is not used for detection:
> `EZ3.` and `EZ9.` are shared between the MC micro EZA radios and the Radius M110s, separated only
> by the generation digits (`EZ9.00.` against `EZ9.01.00.45`).

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
