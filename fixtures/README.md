# Fixtures

Codeplug images the conformance suite needs, kept here so the repository is self-contained.
Every one is either a factory default recovered from the original software or a read of a real
radio; none contains Motorola code.

| File | What it is | Provenance |
|---|---|---|
| `eva9_real.bin` | 512-byte codeplug of a real MC micro EVA 9 (SEL5), VHF band 2, 24 channels programmed, test channel 145.175 MHz | reconstructed from the hardware read session `captures/mcm_read_eva9_*.txt` |
| `eva9_ident.bin` | the **41-byte** ident the radio returns to the **`*`** command: `EV9.01.00.11 455M11-3     5/6 Tone radio` + 0x1A | same capture, nibble-decoded |
| `ev9_default.bin` | **the factory default codeplug for the SEL5 EVA family** — what `MCEV9R`'s `INITIALIZE` writes to a blank radio | reconstructed from `MCEV9R_rt.bin`, see below |
| `eza9_default_band[1-4].bin` | **factory default codeplugs for the SEL5 EZA 9**, 256 bytes each, one per RF range | captured from `MCEZ9R`'s `INITIALIZE`, see below |
| `ez13_default_band[1-4].bin` | **factory default codeplugs for the CS/PL EZA 1/3**, 128 bytes each (the `1 x 128` EEPROM configuration) | captured from `MCEZ13R`'s `INITIALIZE` |

## `ev9_default.bin`

`MCEV9R` (the 1989 *repair* build) has a main-menu item the ordinary build lacks:
`INITIALIZE - 4 (reset to default)`. It runs a short overlay procedure that does exactly:

```
DS:0x810 := $1FF                                   ; 512-byte device
for i := 0 to $E7 do Eeprom[i] := CS:[$31F8+i]     ; 232 bytes -> 0x000..0x0E7
FillChar(Eeprom[$E8], $2FD, $FF)
for i := 0 to $1A do Eeprom[$1E5+i] := CS:[$32E0+i]; 27 bytes  -> trakmode 0
```

so the whole default is one 259-byte blob at CS:0x31F8..0x32FA. The blob is **byte-identical in the
ordinary `MCEV9` build**, which carries it but has no code that reads it.

The stored blob leaves byte 0x000 = 0xFF (a placeholder; the image sums to 0x95). The fixture here
has the checksum corrected to **0x69** so it satisfies `sum(512) mod 256 == 0xFF` and can be served
to the software directly.

**Why it is worth keeping**: fed to the original editor it renders as a coherent all-STD
configuration — ZVEI STANDARD, 140 ms pretime, 60 s time-out, 7 s auto reset, 50 ms synth lock,
08 CHANNEL — with every value landing where `EEPROM_MAP_EV9.md` says it should. That is an
independent end-to-end check of the map, from a source that had no part in building it. It is also
the starting image a modern programmer needs in order to initialise a blank radio.

`0x0DC` is left 0xFF, whose bits 4-6 = 7 is the documented "unprogrammed" sentinel, so the editor
asks for the RF range immediately after INITIALIZE — which is exactly what it does in practice.

Both are **observations of real hardware**, not derived from the disassembly, which is why they are
worth keeping under version control: they are the only ground truth in the repository.

`tools/ev9.py` masks `eva9_real.bin`'s `0x0AC` low nibble to 0 and fixes the checksum before serving
it, because the SEL5 EVA editor rejects a read whose type nibble is not 0 or 1.


## `eza9_default_band[1-4].bin`

The EZA editor refuses to open until a read succeeds, and it rejected every codeplug we had with
`ERROR : TYPE EVA ?` — it identifies the model by the reference dividers, which the EZA keeps at
`0x0C4` and the EVA at `0x0D4`, so it correctly recognised our EVA images and declined them.

`MCEZ9R`, the repair build, has `INITIALIZE - 4 (reset to default)` on its main menu. It
synthesises a genuine EZA codeplug and writes it straight to the radio, so these four were captured
from the wire without owning an EZA — `tools/eza.py`, `default_codeplug()`.

Each is **256 bytes** and satisfies the same checksum rule as the EVA images
(`sum mod 256 == 0xFF`). They differ only in `0x082` (band index in bits 4-6, raster in bit 7),
`0x07A` (synthesiser lock time) and the checksum. Band 1 additionally asks `CHANNEL SPACING`; the
stored file is the 12.5 kHz answer, which sets `0x082` bit 7 (20 and 25 kHz clear it).

These unblocked the whole EZA family — see `doc/EEPROM_MAP_EZA.md`.


## A correction to `eva9_ident.bin` (2026-08)

This fixture previously held **42** bytes beginning `7EV9…`, described as the reply to `)01`. Both
parts were wrong, and re-reading the raw per-byte capture settled it:

* The ident is returned by **`*`** (0x2A), nibble-encoded on the wire (82 chars → 41 bytes).
  `)01` **always** returns a single byte, `eeprom[addr]` — the capture's two `)01` calls return
  `0x36` and `0x73`, plainly codeplug data rather than a constant.
* The leading `0x37` was therefore a codeplug byte from a preceding `)01`, not part of the ident.
  It reads plausibly as a type prefix ("7EV9…"), which is why it survived unquestioned.

The correction is self-confirming: with `tools/radiosim.py` fixed to match, the emulated 1987
editor now **prints the ident on screen** during a read, which it never did before, and its command
order (`)01` → `*` → `)01` → reads) matches the capture exactly.


## `eza9_programmed.bin` — an EZA9 with known frequencies (2026-08)

256 bytes, MCEZ9, band 2. Written by the original `MCEZ9R` editor under emulation after typing
**TX 136.0000 / RX 174.0000** into channel 1 (`tools/eza.py` machinery; capture script kept in the
session scratchpad, the recipe is the `EZA9` case of `clock_shift()` in
`tools/verify_behaviour.py`).

It exists because the two EZA factory-default fixtures cannot distinguish the halves of a channel
record. Every channel in them is *unprogrammed*, so the TX triplet at `+0` and the RX triplet at
`+3` are both all-zero, and a decoder that swapped them would produce identical output. This was
found by mutation-testing the C suite: changing the EZA RX offset from `+3` to `+2` left all 613
assertions passing. With this fixture the same mutation fails, as do `+0`→`+1`, the stride, the
slot count, and the band and reference-divider offsets.

Channel 1 reads `de2000` / `5e6220` → 136.000000 MHz and an LO of 152.600000 MHz, which is
174.000000 − 21.4 MHz. The `0x80` difference between the two `b0` bytes is the clock-shift bit,
answered `N` here, consistent with its documented position at bit 7 of the RX half on this model.
