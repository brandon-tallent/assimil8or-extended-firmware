# Changelog

Versions are MAJOR.MINOR.PATCH. Major: breaks presets or the editor
protocol. Minor: features. Patch: fixes. The version is shown in the patcher
and under **Utilities > About this Module**.

## Unreleased

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
