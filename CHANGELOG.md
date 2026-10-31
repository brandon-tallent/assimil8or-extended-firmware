# Changelog

Versions are MAJOR.MINOR.PATCH. Major: breaks presets or the editor
protocol. Minor: features. Patch: fixes. The version is shown in the patcher
and under **Utilities > About this Module**.

## Unreleased

## 1.2.2 — 2026-10-31

### Added

- Editors can change every mix effects setting over MIDI (Set IDs
  200–246).
- Editors can read each compressor's gain reduction and the delay's tempo
  live (ID 247).
- Editors can read all mix effects settings at once (stream kind 7).
- A remote edit redraws an open mix effects page.
- The preset summary ends with a checksum of the mix effects settings.

### Fixed

- A compressor driving a channel to full scale with Mix at 100% wrapped
  into harsh distortion.

## 1.2.1 — 2026-10-30

### Changed

- Folder loads are much faster: a folder of 449 WAVs loads in under
  2 seconds instead of nearly 50.

## 1.2.0 — 2026-10-30

### Added

- Mix effects: press Pan/Mix twice.
- A compressor on each channel and on the mix outputs, with Drive, Gain
  and Mix.
- Compressor Sidechain from any channel or the Sample input.
- Reverb, gated reverb and chorus algorithms modeled on the Roland DEP-5.
- A tempo-synced ping-pong delay.
- Each channel's sends are taken before or after its level and mute.
- Sends and effect settings take CV.

### Presets

- New `MixFX` lines, which 1.1.0 and Rossum's software ignore.

## 1.1.0 — 2026-10-28

### Added

- Time Lock (press Pitch twice): pitch changes without changing a loop's
  timing. Tempo source per channel: Own, Speed, MIDI clock or CV clock.
  1.0.1 and Rossum's software ignore these preset settings.

## 1.0.1 — 2026-10-25

### Changed

- EQ starts Off on every channel; turn it On on the EQ page. A preset saved
  with a channel's EQ untouched now opens with it Off, and 1.0.0 opens
  one saved here with it On; both sound the same, as the EQ is flat.
- The Filter page cursor moves across each row, then down, like the other
  pages.

## 1.0.0 — 2026-10-21

### New

- Stereo filter offset: on a stereo pair, press the Stereo/Right channel's
  button on the Filter page to set the right side's cutoff. It follows the
  Master's cutoff.
- Each channel keeps its own grain settings.
- Hold the encoder while turning for coarse steps.
- Version number on About this Module and in Identify.

### Changed

- New pages step one unit per encoder click, like stock pages. Grain Size
  steps 0.1 ms.
- Filter cutoff and EQ frequencies step 1/120 octave.
- Encoder holds (reset, restore, zone follows channel) act on release.

### Fixed

- Grain Scan, Overlap, Slide and Skew skipped values.
- Fast turns on the Grain page dropped clicks.
- Cutoff and EQ frequencies jumped in large steps and drifted.
- Turning while holding the encoder closed the edit popup, and on Type could
  reset the filter.

### Presets

- Grain settings are saved per channel. Older presets load their grain
  settings into every channel.
- New lines: `ExtrasFormat`, `Grain<c>`, `FILTR`.
- Opening a preset saved by this release in an earlier one loses its grain
  settings and right-side offsets, and any EQ or filter line with a decimal
  frequency.

### Editor protocol

- EQ frequencies (97–99) and cutoff (129) are steps 0–1196.
- Filter records are 16 bytes. Set 141 is the right-side cutoff offset.
- Grain Sets apply to the channel named. Sets 170 and 194 reply that
  channel's values.
- Identify includes the version.

## Before 1.0.0

Unnumbered builds. The last one added the multimode filter.
