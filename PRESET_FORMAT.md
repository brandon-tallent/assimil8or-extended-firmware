# Preset files

Presets are Rossum's `prstNNN.yml` files with extra lines appended for the
EQ, filter, grain and Time Lock settings. Rossum's software ignores these lines and
drops them on save. Lines at their defaults are not written.

The first extra line is `ExtrasFormat : 3`. Files without it are format 1
(before 1.0.0). See [CHANGELOG.md](CHANGELOG.md) for format changes.

## Lines

`<c>` is a channel and `<z>` a zone, both 1–8. Values are space-separated
integers, except frequencies, which may have two decimals.

| Line | Values |
| --- | --- |
| `EQ<c>` | on hpf, then freq gain q for Low, Mid, High |
| `EQCV<c>` | source amount ×7: Low freq, Low gain, Mid freq, Mid gain, High freq, High gain, HPF |
| `FILT<c>` | type cutoff resonance extra |
| `FILTCV<c>` | source amount ×4: cutoff, resonance, extra, type |
| `FILTR<c>` | right-side cutoff offset (stereo pairs) |
| `Grain<c>` | on size overlap spray detune octave reverse pan shape scan slide skew |
| `GrainCV<c>` | source amount ×8: size, overlap, spray, detune, slide, scan, shape, pan |
| `TimeLock<c>` | on [tempo speed input pulses]: on is 1 for Time Lock on; the rest, written when any is set, as below |
| `TimeLock<c>Z<z>` | beats: the zone's loop length in beats, 1–128 (written only when set; without the line, Auto) |

Add `Z<z>` (for example `EQ1Z2`, `FILTR1Z2`) for a zone with its own EQ or
filter.

Units:

- Frequencies and cutoff: Hz. HPF: Hz, 0 for off.
- EQ gain: 0.1 dB. EQ q: 0 for the band's default shape, else 1–41.
- Filter type: 0 Off, 1 LP 24, 2 LP 12, 3 BP 24, 4 BP 12, 5 HP 24, 6 HP 12,
  7 Notch 24, 8 Notch 12, 9 Band Stop, 10 Dual L/BP.
- `FILTR`: cents.
- Grain size: 0.1 ms. Spray: ms. Detune: cents. Slide: 0.1%. Others: %.
- CV source: −4 Off, −3 to −1 the channel's CV A–C, 0–23 inputs 1A–8C.
  Amount: −100 to 100.
- Time Lock tempo: 0 channel 1's (on channel 1, Own), 1 Own, 2 Speed,
  3 MIDI clock, 4 CV clock. Speed: 0.1%
  from 100%, −750 to 3000. Input: the CV clock's source, as CV source.
  Pulses: the CV clock's pulses a beat, 0 for 4. Absent numbers are 0, and
  input −4.

Format 1 used `Grain` and `GrainCV` with no channel number: the playing
channel (0 for none) followed by the `Grain<c>` values. They load into all
eight channels.

Format 2 is format 3 without the `TimeLock` lines.

```yaml
  ExtrasFormat : 3
  EQ1 : 1 80 150 30 0 1000 -45 12 10000 0 0
  EQCV1 : -4 100 -4 100 0 50 -4 100 -4 100 -4 100 -4 100
  FILT1 : 1 800 55 50
  FILT1Z1 : 6 3000 55 50
  Grain1 : 1 800 400 20 0 0 25 50 100 0 0 -60
  TimeLock1 : 1 3 0 -4 0
  TimeLock2Z1 : 8
```
