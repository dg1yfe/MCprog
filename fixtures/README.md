# Fixtures

Codeplug images the conformance suite needs, kept here so the repository is self-contained. Each is
either a factory default recovered from the original software or a read of a real radio. None
contains Motorola code.

Provenance of the form `reports/run…` names a hardware run record kept locally and **not committed**
— those carry radio serial numbers and further codeplug contents. The citation identifies the
session an image came off; it is not a path in this repository.

## Index

| file | bytes | model | contents | provenance |
|---|---|---|---|---|
| `eva9_real.bin` | 512 | EVA 9 SEL5 | VHF band 2, 24 channels, test channel 145.175 MHz | hardware read, `captures/mcm_read_eva9_*.txt` |
| `eva9_ident.bin` | 41 | EVA 9 | reply to `*`: `EV9.01.00.11 455M11-3     5/6 Tone radio` + `0x1A` | same capture, nibble-decoded |
| `ev9_default.bin` | 512 | EVA SEL5 | factory default | `MCEV9R` `INITIALIZE`, reconstructed from `MCEV9R_rt.bin` |
| `eza9_radio.bin` | 256 | EZA 9 | VHF band 2, 8 channels all TX 149.85000 / RX 154.45000 MHz | hardware read, `mcprog --selftest` |
| `eza9_programmed.bin` | 256 | EZA 9 | band 2, channel 1 TX 136.00000 / RX 174.00000 MHz | written by `MCEZ9R` under emulation |
| `eza9_default_band[1-4].bin` | 256 | EZA 9 SEL5 | factory defaults, one per RF range | `MCEZ9R` `INITIALIZE`, captured off the wire |
| `ez13_default_band[1-4].bin` | 128 | EZA 1/3 CS/PL | factory defaults, one per RF range | `MCEZ13R` `INITIALIZE`, verbatim |
| `m110_cspl_radio.bin` | 256 | M110 CSQ/PL `EZ3.01.00.44` | 70 cm, 2 channels TX 438.61250 / RX 431.01250 MHz, differing only in PL | hardware read, `reports/run3-EZA3,70cm,2Ch/` |
| `m110_cspl_2m_radio.bin` | 256 | M110 CSQ/PL `EZ3.01.00.44` | 2 m, 2 channels TX/RX 144.80000 MHz, 123.0 Hz PL | hardware read, `reports/run4-EZA3,2m,CTCSS,2Ch/` |
| `m110_sel5_radio.bin` | 256 | M110 Sel 5 `EZ9.01.00.45` | 70 cm, 1 channel 439.98750 MHz | hardware read, `reports/run5-EZA9,70cm/` |
| `m110_sel5_2m_radio.bin` | 256 | M110 Sel 5 `EZ9.01.00.45` | 2 m, 1 channel 144.80000 MHz, 5-tone data programmed | hardware read, `reports/write-runs/report8.md.dat` |

Both CSQ/PL images are **two identical 128-byte copies** as the device returns them (K-25); the
codeplug is the first 128. Both Sel 5 images are genuine 256-byte codeplugs, not mirrored.

Four M110 images, two per variant, at two bands each. `m110_cspl_2m_radio.bin` is the only 2 m
CSQ/PL, which is what makes `0x00A & 0x0F` testable as the band field across both variants: `0x12`
reads `0x32` there and `0x00` on both Sel 5 radios, so `0x12` is not the band.

## `ev9_default.bin`

`MCEV9R`'s `INITIALIZE - 4 (reset to default)` runs:

```
DS:0x810 := $1FF                                   ; 512-byte device
for i := 0 to $E7 do Eeprom[i] := CS:[$31F8+i]     ; 232 bytes -> 0x000..0x0E7
FillChar(Eeprom[$E8], $2FD, $FF)
for i := 0 to $1A do Eeprom[$1E5+i] := CS:[$32E0+i]; 27 bytes  -> trakmode 0
```

The default is therefore one 259-byte blob at `CS:0x31F8..0x32FA`, byte-identical in the ordinary
`MCEV9` build, which carries it but has no code that reads it.

The stored blob leaves `0x000` = `0xFF` as a placeholder and sums to `0x95`. **This fixture has the
checksum corrected to `0x69`**, so it satisfies `sum(512) mod 256 == 0xFF` and can be served
directly.

`0x0DC` is `0xFF`; bits 4-6 = 7 is the unprogrammed sentinel, so the editor asks for the RF range
immediately after `INITIALIZE`.

Decoded configuration: ZVEI STANDARD, 140 ms pretime, 60 s time-out, 7 s auto reset, 50 ms synth
lock, 08 CHANNEL.

## `eva9_real.bin`

`tools/ev9.py` masks `0x0AC`'s low nibble to 0 and fixes the checksum before serving it: the SEL5
EVA editor rejects a read whose type nibble is not 0 or 1.

## `eza9_default_band[1-4].bin`

Synthesised by `MCEZ9R`'s `INITIALIZE` and captured off the wire — `tools/eza.py`,
`default_codeplug()`. Each satisfies `sum mod 256 == 0xFF`.

They differ only in:

| offset | field |
|---|---|
| `0x082` | band index in bits 4-6, raster in bit 7 |
| `0x07A` | synthesiser lock time |
| — | checksum |

Band 1 additionally prompts `CHANNEL SPACING`; the stored file is the 12.5 kHz answer, which sets
`0x082` bit 7. 20 and 25 kHz clear it.

## `eza9_programmed.bin`

Exists because every channel in the EZA factory defaults is unprogrammed: the TX triplet at `+0` and
the RX triplet at `+3` are both all-zero, so a decoder that swapped them produces identical output.
This fixture has distinct values and fails that mutation, as it does `+0`→`+1`, the stride, the slot
count, and the band and reference-divider offsets.

Channel 1 reads `de2000` / `5e6220` → 136.000000 MHz and an LO of 152.600000 MHz = 174.000000 −
21.4 MHz. The `0x80` difference between the two `b0` bytes is the clock-shift bit, answered `N`,
at bit 7 of the RX half on this model.

## `ez13_default_band[1-4].bin`

Exactly what `MCEZ13R`'s `INITIALIZE` puts on the wire: 128 bytes beginning
`00 00 81 ED 16 81 12 01`. Served verbatim they read `READING OK` and write `WRITING OK` on all four
bands, with the editor's checksum byte untouched and the whole 128 bytes summing to `0xFF`.

| item | offset |
|---|---|
| checksum byte | `0x003`, covering all 128 bytes |
| reference dividers | `0x004` |

**Open:** the MCEZ13 ident is **synthetic**. It satisfies every check the 1987 software makes, but no
MCEZ13 radio has been read, so `0x000`–`0x001` are unconfirmed.

## `eza9_radio.bin`

Hardware read over the hand-built interface. 4 records then a bare NAK at `0x0100`; stored checksum
`0xFB`, total `0xFF`; reference dividers `1681 1201` at `0x0C4`; band index 2 with the raster bit
set; 8 channels × 6 bytes at `0x0C8`, all carrying the same pair; auto-ack delay `0x0D` = 203 ms at
`0x076`.

Its ident is **37 bytes** — `EZ9.00.02.03 Copr,1987 Motorola GmbH` + `0x1A` — four shorter than the
EVA's. Ident length is per-model; `0x1A` is the terminator to trust (`P-20`).

All eight channels carry the same pair, so this exercises a fully programmed table with no
terminator. `eza9_programmed.bin` is the one with distinct frequencies per slot.
