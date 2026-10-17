# Preset files

An Assimil8or preset is a text file, `prstNNN.yml`, in its folder on the
card. This firmware reads and writes Rossum's format unchanged and adds a
few lines of its own for the EQ and the grain settings. This document
describes those lines, then the stock keys as the v3.04 software reads
them, for anyone writing preset files or an editor.

## The added lines

They follow the stock preset body. Each is a key, a colon and integers.
Rossum's software loads a preset without them, since their keys match none
of its own, and leaves them out when it saves. A preset whose EQ and grain
settings are all at their defaults has none, and is exactly the file
Rossum's software writes.

| Line | Values |
| --- | --- |
| `EQ<c>` | Channel c's EQ: On (0 or 1), HPF in Hz (0 Off, else 20–350), then for Low, Mid and High the frequency in Hz (20–20000), the gain in 0.1 dB (±180 for a bell, ±100 for a shelf) and the Q step (0 the band's own shape, 1–41 a bell of Q = 0.5 · 2^((q − 1) / 8)) |
| `EQCV<c>` | Channel c's seven CV routes, a source and an amount each, for Low frequency, Low gain, Mid frequency, Mid gain, High frequency, High gain and HPF |
| `EQ<c>Z<z>`, `EQCV<c>Z<z>` | The same for zone z of channel c when it has its own EQ. A zone without an `EQ<c>Z<z>` line follows its channel |
| `Grain` | The grain channel (1–8, or 0 for off), then Size (0.1 ms, 5–20000), Overlap (percent, 25–800), Spray (ms, 0–2000), Detune (cents, 0–1200), Octave, Reverse and Pan (percent, 0–100), Shape (percent, 1–100), Scan (percent, ±400), Slide (0.1 percent of the zone, ±1000) and Skew (±100) |
| `GrainCV` | Eight CV routes, a source and an amount each, for Size, Overlap, Spray, Detune, Slide, Scan, Shape and Pan |

Channels and zones count from 1. A CV source is −4 for Off, −3 to −1 for
the channel's own CV A, B and C inputs, and 0 to 23 for the inputs 1A, 1B,
1C, 2A … 8C. An amount is −100 to 100, for −1.00 to +1.00.

Keys are not case-sensitive. A line with a channel or zone outside 1–8, or
with too few numbers, is ignored; values outside their ranges are brought
inside them. Only lines that differ from the defaults are written. The
defaults are EQ On, HPF Off, 150 Hz, 1 kHz and 10 kHz at 0 dB with each
band's own shape; every route Off at +1.00; and Grain
`0 600 400 20 0 0 0 50 100 100 0 0`.

```yaml
Preset 1 :
  Name : Breaks
  Channel 1 :
    PitchCV : 0A 0.50
    Zone 1 :
      Sample : Amen.wav
  EQ1 : 1 80 150 30 0 1000 -45 12 10000 0 0
  EQCV1 : -4 100 -4 100 0 50 -4 100 -4 100 -4 100 -4 100
  EQ1Z1 : 1 0 150 0 0 1000 0 0 8000 -60 0
  Grain : 1 800 400 20 0 0 25 50 100 0 0 -60
  GrainCV : -4 100 -4 100 -4 100 -4 100 5 100 -4 100 -4 100 -4 100
```

Here channel 1 has an 80 Hz high-pass, +3 dB at 150 Hz and a −4.5 dB bell
at 1 kHz whose frequency follows input 1A at half depth; its first zone has
its own EQ; and channel 1 plays 80 ms grains held where the note began,
with Slide following input 2C.

## The stock keys

A file is a `Preset N :` line, the preset's keys, then for each channel in
use a `Channel N :` line, the channel's keys and its `Zone N :` blocks.
Keys are not case-sensitive, and the software skips keys it does not know
and lines it cannot read. When it saves it leaves out most keys that are
at a new preset's values. Indentation is two spaces a level and is not
significant to the reader.

### Preset

| Key | Value |
| --- | --- |
| `Name` | Up to 12 characters |
| `Data2asCV` | A CV input, `1A`–`8C` |
| `MidiSetup` | 0–8 |
| `XfadeACV` … `XfadeDCV` | A CV input for each crossfade group, or `Off` |
| `XfadeAWidth` … `XfadeDWidth` | 0.01–10.00 |

### Channel

| Key | Value |
| --- | --- |
| `Pitch` | Semitones, −96.00 to 60.00 |
| `Level`, `MixLevel` | dB, −90.0 to 6.0 |
| `Pan` | −1.00 to 1.00 |
| `Bits` | 1.0 to 32.0 |
| `Aliasing` | 0 to 100 |
| `Attack`, `Release` | Seconds, 0 to 99 |
| `AttackFromCurrent`, `Reverse`, `LoopLengthIsEnd`, `MixModIsFader`, `LinAMisExtEnv`, `SpliceSmoothing`, `AutoTrigger` | 0 or 1 |
| `PlayMode` | 0 Gate, 1 One Shot |
| `LoopMode` | 0 to 2 |
| `ChannelMode` | 0 Master, 1 Link, 2 Stereo/Right, 3 Cycle |
| `PMSource` | 0 to 10 |
| `PMIndex` | 0 to 1.00 |
| `ZonesCV` | A CV input |
| `ZonesRT` | 0 Gate Rise, 1 Continuous, 2 Random, 3 Advance |
| `XfadeGroup` | `A` to `D` |
| `MixMod` and the sixteen modulations | A CV input and an amount, as `1A 0.50` |

The sixteen modulations are `LinFM`, `ExpFM`, `PitchCV`, `PhaseCV`,
`PMIndexMod`, `BitsMod`, `AliasingMod`, `LinAM`, `ExpAM`, `AttackMod`,
`ReleaseMod`, `PanMod`, `SampleStartMod`, `SampleEndMod`, `LoopStartMod` and
`LoopLengthMod`. A CV input is written `0A`–`0C` for the channel's own
inputs, `1A`–`8C` for a numbered one, or `Off`; an amount is −1.00 to 1.00.

### Zone

| Key | Value |
| --- | --- |
| `Sample` | The WAV file's name in the preset's folder, up to 47 characters |
| `Side` | 0 or 1: which side of a stereo file a mono channel plays |
| `MinVoltage` | Volts, −5.00 to 5.00 |
| `SampleStart`, `SampleEnd`, `LoopStart` | Frames |
| `LoopLength` | Frames, with a fraction: at least 4 |
| `PitchOffset` | Semitones, −96.00 to 60.00 |
| `LevelOffset` | dB, −90.0 to 6.0 |

A channel's zones in use are the ones before the first without a `Sample`.
