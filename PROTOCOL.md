# Remote control protocol

The Tallent Edition firmware answers System Exclusive messages on the
Assimil8or's MIDI connection (Rossum's Locutus expander). With them a
computer or sequencer can read and edit the working preset, browse the card,
audition and assign samples, drive the front panel, read the screen, record,
and install software. This document is the whole wire contract, for anyone
writing an editor.

It describes protocol version 1, as Identify reports it.

- [Messages](#messages)
- [Example](#example)
- [Commands](#commands)
- [Identity and health](#identity-and-health)
- [Screen and panel](#screen-and-panel)
- [Preset editing](#preset-editing)
- [Parameters](#parameters)
- [Streams](#streams)
- [Automation](#automation)
- [Sample upload](#sample-upload)
- [Recording](#recording)
- [Software update](#software-update)
- [Developer commands](#developer-commands)
- [The working model](#the-working-model)
- [Notes for editor authors](#notes-for-editor-authors)

## Messages

Every request is one System Exclusive message:

```text
F0 7D 41 38 01 <command> <sequence> <length> <packed payload> F7
```

| Byte | Meaning |
| --- | --- |
| `7D` | The noncommercial manufacturer ID |
| `41 38` | `A8`: tells these messages from other devices that use `7D` |
| `01` | Protocol version |
| command | `01`–`3F` |
| sequence | `00`–`7F`, chosen by the sender and echoed in the reply |
| length | The payload's byte count before packing: at most 48 in a request, 127 in a reply |

**Packing.** A payload's bytes are sent seven at a time. Each group is one
byte holding the group's high bits (bit 0 is the first byte's, bit 1 the
second's, and so on), then the bytes themselves with their high bits clear.
The last group may be shorter; unused high-bit positions must be zero. A
payload of n bytes therefore takes n + ⌈n / 7⌉ bytes on the wire, and the
longest request is 64 MIDI bytes in all.

**Replies.** A reply uses the same frame with `command | 40h` and the
request's sequence. Its payload begins with a result byte, and what follows
is described with each command. Integers are little-endian throughout. `u8`,
`u16` and `u32` are unsigned, `s8`, `s16` and `s32` signed.

| Result | Meaning |
| ---: | --- |
| 0 | Success |
| 1 | Unknown command, or the extension that serves it is not loaded |
| 2 | Wrong payload length |
| 3 | A value is out of range |
| 4 | Busy: nothing was done; ask again |
| 5 | Storage error |
| 6 | CRC mismatch |
| 7 | Invalid software image |
| 8 | Not possible in the current state |
| 9 | Out of memory |
| 10 | A file of that name already exists |

A failed request replies with its result and no data. A frame that fails the
framing rules (wrong header, a length that does not match, a set high bit
that packing does not allow, more than 48 payload bytes) is dropped without
a reply.

**Timing.** MIDI carries 3,125 bytes a second, so pace what you send to
that. Send one request and wait for its reply before the next: the preset
editing commands hold a single request at a time and answer Busy to a
second. Most replies follow at once; those that touch the card are noted
below and can take seconds.

**Sharing the cable.** Notes, controllers and realtime bytes can be sent
freely between requests, and realtime bytes (`F8`–`FF`) may fall inside a
frame in either direction: strip them before decoding. The module's MIDI
output also carries its ordinary messages and the replies to any other
sender's requests, so match replies by command and sequence and ignore the
rest. Channel and zone numbers on the wire count from zero.

## Example

Identify, then read the preset's name. This is complete and runs against a
module with [python-rtmidi](https://pypi.org/project/python-rtmidi/); change
the port name to yours.

```python
import struct, time
import rtmidi

HEADER = bytes((0xF0, 0x7D, 0x41, 0x38, 0x01))

def pack(command, sequence, payload=b""):
    frame = bytearray(HEADER + bytes((command, sequence, len(payload))))
    for at in range(0, len(payload), 7):
        group = payload[at:at + 7]
        frame.append(sum((byte >> 7) << bit for bit, byte in enumerate(group)))
        frame.extend(byte & 0x7F for byte in group)
    return bytes(frame) + b"\xF7"

def unpack(frame):
    command, sequence, length = frame[5:8]
    body, payload = frame[8:-1], bytearray()
    while len(payload) < length:
        high, group, body = body[0], body[1:8], body[8:]
        payload.extend(byte | ((high >> bit) & 1) << 7 for bit, byte in enumerate(group))
    return command, sequence, bytes(payload[:length])

def request(port_out, port_in, command, payload=b"", sequence=1):
    port_out.send_message(list(pack(command, sequence, payload)))
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        message = port_in.get_message()
        if not message:
            time.sleep(0.001)
            continue
        frame = bytes(byte for byte in message[0] if byte < 0xF8)   # realtime bytes may sit inside a frame
        if frame[:5] == HEADER and frame[5] == command | 0x40 and frame[6] == sequence:
            result, *data = unpack(frame)[2]
            if result:
                raise RuntimeError(f"result {result}")
            return bytes(data)
    raise TimeoutError

out, inp = rtmidi.MidiOut(), rtmidi.MidiIn()
inp.ignore_types(sysex=False)
out.open_port(out.get_ports().index("M2")); inp.open_port(inp.get_ports().index("M2"))

reply = request(out, inp, 0x01)                                          # Identify
magic, protocol, build, crc, boot, failed, ext_crc, ext_state = struct.unpack("<8sI8sIIIIB", reply)
print(magic.decode(), "protocol", protocol, "build", build.hex())

reply = request(out, inp, 0x33, struct.pack("<HB", 0, 12), sequence=2)   # Read model: offset 0, 12 bytes
print("preset name:", reply.decode())
```

The Read model request above is `F0 7D 41 38 01 33 02 03 00 00 00 0C F7`:
command `33`, sequence `02`, three payload bytes, one group with no high
bits set, then `00 00` (offset 0) and `0C` (twelve bytes).

## Commands

| Hex | Command | Group |
| --- | --- | --- |
| 01 | Identify | [Identity and health](#identity-and-health) |
| 02 | Diagnostics | |
| 07 | Foreground progress | |
| 08 | Display mode | [Screen and panel](#screen-and-panel) |
| 09 | Panel | |
| 0A | Screenshot | |
| 32 | Preset summary | [Preset editing](#preset-editing) |
| 33 | Read model | |
| 34 | Sample info | |
| 35 | Set parameter | |
| 36 | Write zone | |
| 37 | Assign sample | |
| 38 | List folder, Open folder | |
| 39 | Zone peaks | |
| 3A | Transients | |
| 3B | Explode | |
| 3C | Save preset | |
| 3D | File peaks | |
| 3E | Stop channel | |
| 3F | Stream | [Streams](#streams) |
| 2D | EQ automate | [Automation](#automation) |
| 20–23 | Begin, data, commit and abort sample | [Sample upload](#sample-upload) |
| 28–2C | Record arm, start, stop, cancel, status | [Recording](#recording) |
| 10–1A | Install, promote, restore, reboot | [Software update](#software-update) |
| 03–06 | Peek, poke, stream | [Developer commands](#developer-commands) |

Commands 32–3F are served by the extension image, the second file the
patcher gives you. Without it they reply result 1, and Identify says so.

The commands under Developer are for working on the firmware itself. They
are documented because they are there; they are not a stable interface.

## Identity and health

**01 Identify.** No payload. Reply:

| Field | |
| --- | --- |
| 8 bytes | `SQA8PTCH` |
| u32 | Protocol version |
| 8 bytes | Build ID: names this exact software image |
| u32 | CRC-32 of the software image |
| u32 | Boot mode: 0 stable, 1 trial (see [Software update](#software-update)) |
| u32 | Nonzero when the last trial failed and the module fell back |
| u32 | CRC-32 of the loaded extension, 0 when none |
| u8 | Extension state: 0 untried, 1 loaded, 2 missing, 3 invalid, 4 storage error |

A module running Rossum's own software does not answer. Check the protocol
version and that the extension is loaded before using commands 32–3F.

**02 Diagnostics.** Empty payload, or one byte `01` to clear the renderer
timing figures first. Reply: 31 u32 counters in this order.

| Counters | |
| --- | --- |
| `uptime_ticks` | About 1.04 ms each |
| `panel_polls`, `foreground_polls` | Passes of the panel service and the foreground |
| `rx_bytes`, `rx_sysex_frames`, `rx_sysex_rejects`, `rx_sysex_overflows`, `rx_stray_data` | MIDI received; rejects are frames that failed the framing rules |
| `tx_sysex_bytes`, `tx_stock_bytes`, `tx_frame_drops` | MIDI sent; a drop is a reply that found the output queue full |
| `render_calls` | Audio frames rendered |
| `render_cycles_last`, `_min`, `_max` | CPU cycles one frame took |
| `render_interval_last`, `_min`, `_max` | Cycles between frames |
| `render_over_budget` | Frames that took 20,800 cycles or more |
| `render_load[8]` | Frames by cost, in bins of 2,048 cycles |
| `wdt_kicks`, `heap_break`, `reset_status`, `last_job_result` | Watchdog feeds, the heap's top, the processor's reset cause, the last update job's result |

**07 Foreground progress.** No payload. Reply: u32 successful disk reads by
the foreground, u32 the stock sample service's state (0 idle). An installer
uses it to see that a starting module is making progress.

## Screen and panel

**08 Display mode.** One byte: 0 stock colors, 1 white text, 2 white text,
lines, meters and waveforms. It lasts until restart; Utilities >
Preferences > High Contrast is the saved setting.

**09 Panel.** Presses buttons and turns the encoder as a hand would:
u32 button mask, u16 hold time in ticks, s8 encoder steps. The buttons are
pressed together for the hold time and released, then the encoder turns.
The reply comes when the stock dispatcher has finished with them. A zero
mask turns the encoder only. Holds are limited to 5,000 ticks and steps to
±64; a second request while one runs replies Busy. About 100 ticks makes a
press and 1,000 a hold.

| Bit | Button | Bit | Button | Bit | Button | Bit | Button |
| ---: | --- | ---: | --- | ---: | --- | ---: | --- |
| 0–7 | Channel 1–8 | 12 | Sample | 17 | Phase Mod | 22 | Loop Start |
| 8 | Load | 13 | Sampling Setup | 18 | Mutate | 23 | Loop End |
| 9 | Save | 14 | Zones | 19 | Pan/Mix | 24 | Play Mode |
| 10 | Utility | 15 | Pitch | 20 | Sample Start | 25 | Envelope |
| 11 | Channels | 16 | Level | 21 | Sample End | 26 | Loop Mode |
| | | | | | | 27 | Encoder switch |

**0A Screenshot.** With no payload the module takes a snapshot of the
display and replies the encoded size as a u32. Then read it in order with
u16 offset and u8 count (1–126), which reply that many encoded bytes;
reading the last ends the transfer. A new snapshot replaces an unread one.

The image is 128 × 128 pixels, two bytes each (RGB565, high byte first),
in pixel PackBits. Each token byte's low seven bits plus one is a count of
1–128 pixels. With bit 7 set, the next two bytes are one pixel repeated that
many times; otherwise that many two-byte pixels follow.

## Preset editing

These commands read and change the working preset: what the panel shows and
the module plays. An edit stores what the front panel's own control would
store and takes effect at once. It is not saved until Save preset. The stock
unsaved-changes indicator follows remote edits as it follows panel edits.

The module runs one of these requests at a time, between its own preset
loads and sample jobs. While one runs, another replies Busy.

**32 Preset summary.** No payload. Reply:

| Field | |
| --- | --- |
| u8 | Preset number, 1–199 |
| u8 | Flags: bit 0 unsaved changes, bit 1 the card is busy |
| u32 | CRC-32 of the working model, all 5,728 bytes |
| 12 bytes | Preset name, space padded |
| 8 × u8 | Each channel's Mute/Solo: 0 normal, 1 mute, 2 solo |
| text, then `00` | The active folder's name |
| u32 | Checksum of the channel and zone EQ |
| u32 | Checksum of the grain settings |
| u32 | Checksum of the channel and zone filters |

Poll this to follow the module. When a checksum changes, read that part
again. The three last checksums change when the settings do; they are not
computed over the bytes any reply carries.

**33 Read model.** u16 offset, u8 count (1–120). Reply: that many bytes of
[the working model](#the-working-model). Stream kind 0 reads all of it at
once.

**34 Sample info.** u8 channel, u8 zone. Reply: u32 frames, u32 sample
rate, u8 channels, u8 load state (2 when its audio is in memory), u32 an
identity that changes when the zone's sample does. All zero for an empty
zone.

**35 Set parameter.** u8 channel, u8 zone, u8 ID, s32 value. Reply: s32 the
value as stored, after clamping. [Parameters](#parameters) lists the IDs.

**36 Write zone.** Sets a zone's positions and offsets together: u8
channel, u8 zone, u32 sample start, u32 sample end, u32 loop start, u32 loop
length, s16 pitch offset, s16 level offset, s16 minimum voltage, in the
units of the zone parameters. Reply: empty.

**37 Assign sample.** u8 channel, u8 zone, then a file name of 0–47 bytes.
Loads that WAV from the active folder into the zone and replies u32 frames.
A zone just past the last starts as a copy of the one before it. An empty
name deletes the zone, as the Zones page does: later zones move down.

Two bits of the zone byte change the request:

- Bit 7: the name is a file of the browsed folder (see Open folder), which
  need not be the active one. The sample plays from where it is; Save
  preset copies it into the active folder. Result 10 means the active
  folder already has a different file of that name.
- Bit 6: audition. The channel's voice is ended, the file loaded and the
  zone armed; play the zone's note and it sounds once through, even on a
  channel that loops. Stop channel or another Assign ends it.

**38 List folder.** u16 first index. Reply: u16 first, u8 flags (bit 0: this
is the end), then for each entry that fits: u32 frames, u32 sample rate, u8
channels (`FF` for a folder), u8 name length, name. Entries are the browsed
folder's folders, then its WAV files, each sorted by name with runs of
digits in number order, up to 2,048.

**38 Open folder.** u16 `FFFF`, u8 operation, then a name of 0–45 bytes.

| Operation | |
| ---: | --- |
| 0 | Read the same folder again, which finds files added since |
| 1 | The active folder |
| 2 | The card's top level |
| 3 | Enter the named folder |
| 4 | Up one level |
| 5 | Find: followed by u16 first index and 1–43 bytes of text |

Reply: u16 entries, u16 an index (`FFFF` none: after Up, the folder just
left; after Find, the first entry from the given index whose name contains
the text, in either case), u8 flags (bit 0 the card's top level, bit 1 the
active folder), then the folder's path. One folder is browsed at a time; it
starts as the active folder. Paths are at most 60 characters.

**39 Zone peaks.** u8 channel, u8 zone, u32 first frame, u32 frames per
column, u8 columns (1–40). Reply: for each column s8 minimum, s8 maximum,
u8 RMS, of the zone's sample in memory. For drawing waveforms.

**3D File peaks.** u32 first frame, u32 frames per column, u8 columns, u8
zero, then the name of a WAV in the browsed folder. Reply as Zone peaks,
read from the card. A column longer than 64 KiB is estimated from sixteen
pieces spread across it.

**3A Transients.** u8 channel, u8 zone, u8 pieces (2–8). Runs the stock
Chop analysis over the zone's sample and replies pieces + 1 u32 boundary
frames, without changing anything.

**3B Explode.** u8 channel, u8 zone, u8 slices (1–8), then slices + 1
ascending u32 boundary frames. Writes each slice as a zone: a copy of the
source zone starting and looping at its boundary. The channel's other zones
are cleared and the minimum voltages spread over 0–5 V. Reply: empty.

**3C Save preset.** u8 preset number, or 0 for the current one. Samples
assigned from other folders are copied into the active folder first, then
the stock save runs. Reply: u8 the preset number, when the file is written.
Allow two minutes.

**3E Stop channel.** u8 channel, and optionally u8 a zone to arm for an
audition. Ends the channel's voice as Play Mode + channel does on the
panel. Reply: empty.

## Parameters

Set parameter's IDs. The zone byte matters only where noted.

### Channel

| ID | Parameter | Range |
| ---: | --- | --- |
| 0 | Pitch | −9600…6000 cents |
| 1 | Level | −900…60, 0.1 dB |
| 2 | Pan | −100…100 |
| 3 | Bits | 10…320, 0.1 bit |
| 4 | Attack | 0…990,000, 0.1 ms |
| 5 | Release | 0…990,000, 0.1 ms |
| 6 | Play mode | 0 Gate, 1 One Shot |
| 7 | Mix level | −900…60, 0.1 dB |
| 8 | Aliasing | 0…100 |
| 9 | Phase mod index | 0…100 |
| 10 | Phase mod source | 0…10 |
| 11 | Reverse | 0, 1 |
| 12 | Loop mode | 0…2 |
| 13 | Attack from current | 0, 1 |
| 14 | Loop length is end | 0, 1 |
| 15 | Mix mod is fader | 0, 1 |
| 16 + 2n | Modulation n's source | −4…23 |
| 17 + 2n | Modulation n's amount | −100…100 |
| 48 | Channel mode | 0 Master, 1 Link, 2 Stereo/Right, 3 Cycle |
| 49, 50 | Mix level modulation source, amount | as above |
| 51 | Mute/Solo | 0 normal, 1 mute, 2 solo (not saved with the preset) |

The sixteen modulations, n = 0…15: linear FM, exponential FM, pitch CV,
phase CV, phase mod index, bits, aliasing, linear AM, exponential AM,
attack, release, pan, sample start, sample end, loop start, loop length.

A CV source is −4 Off, −3…−1 the channel's own CV A, B and C inputs, and
0…23 the inputs 1A, 1B, 1C, 2A … 8C.

Channel mode regroups the channels as the Channels page does. Stereo/Right
needs a stereo sample on the channel before it and replies result 8
otherwise.

### Zone

The zone byte selects the zone, which must hold a sample.

| ID | Parameter | Range |
| ---: | --- | --- |
| 64 | Pitch offset | −9600…6000 cents |
| 65 | Level offset | −900…60, 0.1 dB |
| 66 | Minimum voltage | −500…500, 0.01 V |
| 67 | Sample start | frames |
| 68 | Sample end | frames |
| 69 | Loop start | frames |
| 70 | Loop length | 1/32 frame, at least 128 |
| 71 | Side | 0, 1 |

Positions clamp to the loaded sample. `7FFFFFFFh` is the stock value for a
position left at the sample's end.

### EQ

The zone byte is a zone 0…7, or 8 for the channel's EQ. A zone follows its
channel until it is given its own EQ; its first edit copies the channel's.
Stereo/Right channels play their master's EQ.

| ID | Parameter | Range |
| ---: | --- | --- |
| 96 | High-pass filter | 0 Off, 20…350 Hz |
| 97, 98, 99 | Low, Mid, High frequency | 20…20,000 Hz |
| 100, 101, 102 | Low, Mid, High gain | 0.1 dB: ±180 for a bell, ±100 for a shelf |
| 103 | EQ On | 0, 1 |
| 104 | Zone has its own EQ | 0 follows the channel again, 1 own |
| 105…111 | CV source for Low frequency, Low gain, Mid frequency, Mid gain, High frequency, High gain, HPF | −4…23 |
| 112…118 | CV amount for the same seven | −100…100 |
| 119, 120, 121 | Low, Mid, High Q | 0 the band's own shape, 1…41 a bell |

A band's own shape is a shelf for Low and High and, for Mid, a bell whose Q
follows its gain. Q step q is a bell of Q = 0.5 · 2^((q − 1) / 8), from 0.5
to 16. A Q that makes a band a shelf also brings its gain inside ±10 dB. At
an amount of 100 a volt moves a frequency one octave or a gain 2 dB.

### Filter

The zone byte is a zone 0…7, or 8 for the channel's filter, and a zone
follows its channel as for the EQ. Stereo/Right channels play their master's
filter.

| ID | Parameter | Range |
| ---: | --- | --- |
| 128 | Type | 0 Off, 1 LP 24, 2 LP 12, 3 BP 24, 4 BP 12, 5 HP 24, 6 HP 12, 7 Notch 24, 8 Notch 12, 9 Band Stop, 10 Dual L/BP |
| 129 | Cutoff | 20…20,000 Hz |
| 130 | Resonance | 0…100: the damping k = 2(1 − r/100)^1.35, at least 0.05: Q 0.5 to 20 |
| 131 | Extra | 0…100: see below |
| 132 | Zone has its own filter | 0 follows the channel again, 1 own |
| 133…136 | CV source for Cutoff, Resonance, Extra, Type | −4…23 |
| 137…140 | CV amount for the same four | −100…100 |

Extra is the type's own setting. Band Stop: how far above the cutoff its
high-pass sits, 0.11 octave a step, so 0 is a notch at the cutoff. Dual
L/BP: where its band-pass sits, 0.11 octave a step from 5.6 octaves below the
cutoff, on it at 50. Both second filters stop at 20 kHz. The other types
ignore Extra, and so does its CV route.

The 24 dB types are two sections in series, and Resonance sets one: LP 24
and HP 24 are a resonant section into one at Q 0.5, BP 24 a resonant
band-pass into a low-pass at Q 0.5, and Notch 24 a notch of twice the Q into
a broad one. Band Stop is a low-pass at the cutoff beside the high-pass, and
Dual L/BP a low-pass beside the band-pass, each at half level; the
band-passes play louder and Notch 12 at half level. At an amount of 100 a
volt moves the cutoff two octaves, every frame, so 5 V crosses the whole
range; Resonance and Extra 20 steps; and Type two types, never to Off.

### Grain

Grain mode belongs to one channel at a time, or to a Master and its
Stereo/Right partner. The zone byte is unused.

| ID | Parameter | Range |
| ---: | --- | --- |
| 160 | Grain On for this channel | 0, 1 (taking it from any other channel) |
| 161 | Size | 5…20,000, 0.1 ms |
| 162 | Overlap | 25…800: grains sounding at once × 100 |
| 163 | Spray | 0…2,000 ms |
| 164 | Detune | 0…1,200 cents |
| 165 | Octave: share of grains an octave up | 0…100 % |
| 166 | Reverse: share of grains played backwards | 0…100 % |
| 167 | Pan: widest spread, reached as grains grow random (a stereo pair only) | 0…100 % |
| 168 | Shape: share of a grain spent fading | 1…100 % |
| 169 | Scan: speed through the sample against the voice | −400…400 % |
| 177 | Slide: added to the start | −1000…1000, 0.1 % of the zone |
| 195 | Skew: Shape's fading split between attack (+) and release (−) | −100…100 |
| 178 + 2n | Route n's CV source | −4…23 |
| 179 + 2n | Route n's CV amount | −100…100 |

The eight routes, n = 0…7: Size, Overlap, Spray, Detune, Slide, Scan,
Shape, Pan. At an amount of 100, five volts sweep Spray, Detune, Scan,
Shape and Pan across their whole range and Slide across the zone's length;
Size and Overlap move an octave a volt.

Three more IDs only read. Send them with any value.

| ID | Reply in place of the stored value |
| ---: | --- |
| 170 | Engine state: u8 channel (`FF` off), u8 stereo pair, u8 grains sounding, u8 the voice's zone; u32 grains started, u32 skipped, u32 stolen (cut short to make room); s32 pitch step (frames per frame, Q25), u32 playhead frame, u32 the zone's first frame, u32 its frames; then parameters 161–169, 177 and 195 as s16 |
| 171 | What is sounding, for a display: u8 channel (`FF` off), u8 zone, u16 zero, u32 the frame grains start at (`FFFFFFFF` while the voice is silent), then eight u32 grains: bits 0–26 the sample frame, 27–29 the window level in eighths, bit 30 backwards, bit 31 sounding |
| 194 | The routes: eight pairs of s8 source, s8 amount |

### Resample

Records a channel's output, after level, envelope, Bits and EQ and before
pan and mix, into one of its own zones.

| ID | |
| ---: | --- |
| 172 | Arm a take. Zone byte: the zone it goes to, or `FF` for the next free one. Value: bits 0–25 the frame count (0 as long as memory allows), the bits above your MIDI latency in ms. Replies the take's file name; then the recorder replies as [Record arm](#recording) once it is ready |
| 173 | Status: u8 state (0 none, 1 recording, 2 placed, 3 on card, 4 failed), u8 channel, u8 zone, u8 retired takes, u32 frames, the take's name |
| 174 | Put a take that is on the card in the zone byte's zone |
| 175 | Delete the zone byte's zone, which holds a take |
| 176 | End the running take at the value's frame count |

Start, stop and cancel a take with the Recording commands. A Master with a
Stereo/Right partner gives a stereo take, any other channel a mono one.

## Streams

**3F Stream.** One small request, many replies: each reply frame carries
the request's sequence, and they arrive one after another until the data is
complete. A new stream replaces a running one. Other commands can run
beside it.

| Kind | Request after the kind byte | Data |
| ---: | --- | --- |
| 0 | u16 offset, u16 length | The working model |
| 1 | u8 channel, u8 zone, u32 first frame, u32 frames per column, u16 columns (≤ 702) | Zone peaks |
| 2 | u32 first frame, u32 frames per column, u16 columns, file name | File peaks |
| 3 | | Every zone's Sample info: 64 records of 14 bytes |
| 4 | Nothing, or u16 first, u16 entries | The browsed folder's listing |
| 5 | | The EQ: 72 records of 33 bytes |
| 6 | | The filter: 72 records of 14 bytes |

Each reply frame's data is u8 kind, u16 index (the byte offset, column,
first entry or first record this frame starts at), then the data. A frame
that fails carries its result and ends the stream.

An EQ record: u16 HPF, 3 × u16 frequency, 3 × s16 gain, u8 On, u8 own EQ,
7 × s8 CV source, 7 × s8 CV amount, 3 × u8 Q. The records run channel by
channel: zones 1–8, then the channel's own.

A filter record: u8 type, u16 cutoff, u8 resonance, u8 extra, u8 own filter,
4 × s8 CV source, 4 × s8 CV amount; the records run as the EQ's.

## Automation

Two messages set values without a reply, so a sequencer can send a steady
stream of them. Every byte is below `80h`, so a fixed template with the
value filled in serves, and no packing arithmetic is needed.

**35 Set parameter with sequence 0.** No request uses sequence 0, so this
is taken as automation:

```text
F0 7D 41 38 01 35 00 07 00 <channel> <zone> <ID> <n0> <n1> <n2> <n3> F7
```

The value is four nibbles, low first, counted up from the parameter's
lowest value: the n-th step above its minimum. The newest value of each
parameter waits in a table of sixteen and is stored between other jobs. It
takes channel parameters 0–47, 49 and 50 (Attack and Release to 6.5 s), a
zone's pitch offset, level offset and minimum voltage, the EQ's CV sources
and amounts (on the channel's EQ), and grain parameters 160–169, 177 and 195.
Sample positions, Side, Channel mode and Mute/Solo are not automated.

**2D EQ automate.** Sets one field of a channel's EQ at once, without
waiting for other work:

```text
F0 7D 41 38 01 2D 00 06 00 <channel> <field> <n0> <n1> <n2> <n3> F7
```

| Field | Value |
| ---: | --- |
| 0, 1, 2 | Low, Mid, High frequency, in cents above 20 Hz (0…11,959) |
| 3, 4, 5 | Low, Mid, High gain, in dB above −18 (0…36) |
| 6, 7, 8 | Low, Mid, High Q (0 own shape, 1…41) |
| 9 | HPF in Hz (0 Off, 20…350) |
| 10 | EQ On |

## Sample upload

Adds a small WAV to the active folder: at most 16,384 bytes in all, 48 kHz,
mono or stereo, 16-, 24- or 32-bit integer PCM, with the plain 44-byte
header and no other chunks. Existing files are never overwritten.

| Hex | Command | Request | Reply |
| --- | --- | --- | --- |
| 20 | Begin sample | u32 file bytes (48–16,384), u32 CRC-32 of the file, name | Empty |
| 21 | Sample data | u32 file offset, 1–44 bytes | u32 next offset |
| 22 | Commit sample | | The path it was saved to |
| 23 | Abort sample | | Empty |

The name is 1–27 letters, digits, underscores or hyphens, then `.wav`. Send
the data in order; an identical last chunk may be sent again if its reply
was lost. Commit checks the CRC and the header, writes the file, reads it
back and only then gives it its name. A transfer left unfinished for 30
seconds is dropped. Use Open folder operation 0 to see the new file.

## Recording

These drive the stock sampler, as Sampling Setup and the Sample button do.
A take lands where Sampling Setup would put it and is written to the card
by the stock firmware.

| Hex | Command | Request | Reply |
| --- | --- | --- | --- |
| 28 | Record arm | u8: destination in bits 0–3, zone destination in bit 4, side in bits 5–6. u8: rate in bits 0–1, latency in ms in bits 2–7. u32 frames to record, at that rate | u32 frame limit, once the recorder is ready |
| 29 | Record start | | Empty |
| 2A | Record stop | | u32 frames, u32 sample rate, the take's name |
| 2B | Record cancel | | Empty |
| 2C | Record status | | See below |

Destination is 0 unassigned, 1–8 a channel, 9 the first empty channel; the
zone destination is as on the Sampling Setup page. Side is 0 left, 1 right,
2 stereo, 3 dual mono. Rate is 0 for 48 kHz, 1 for 96, 2 for 192. Latency
is how long your Start takes to arrive, 1 ms up to 40 at 48 kHz (20 at 96,
10 at 192): the take begins that much before Start is received, so it
begins when you sent it. The take ends by itself at the frame count.

Record status: u8 state (0 idle, 1 preparing, 2 priming, 3 armed, 4
capturing, 5 writing, 6 done), u8 stock input mode, u8 stock arm state, u8
stock sample service state, u8 flags (bit 0 ended by the module, bit 1
cancelled, bit 2 disarmed from the panel), u8 the result of checking the
written file, u8 latency, u8 zero, u32 frames so far, u32 frame limit, then
the take as Record stop replies it.

Arm replies result 8 or Busy while the sampler, the card or an update is in
use. The front panel stays live: its Sample button starts an armed take.

## Software update

The module can take new software over MIDI and try it before keeping it.
An editor does not need this: the patcher's two files install from the card
with Utilities > Load Software.

A new image is staged in memory, written beside the running one, and
started once as a trial under a 20-second watchdog. If the trial runs, the
host promotes it; if it stalls, the module restarts into the software it
had. Notes stop sounding during an update.

| Hex | Command | Request | Reply |
| --- | --- | --- | --- |
| 10 | Begin install | u32 CRC-32 of the whole file; with a u32 size after it, the file is an extension image | Empty |
| 11 | Block CRC | u32 file offset, a multiple of 4,096 | u32 offset, u32 CRC-32 of that block as staged |
| 12 | Begin block | u32 offset, u16 size, u32 block CRC-32 | Empty |
| 13 | Block data | u32 offset, 1–44 bytes | None on success |
| 14 | End block | u32 offset | u32 offset, after the block's CRC is checked |
| 15 | Commit install | | Empty, after the whole file is checked, written and read back |
| 16 | Abort install | | Empty |
| 17 | Promote | `SQA8` | Empty. Only in a trial: makes it the installed software |
| 18 | Restore | `SQA8` | Empty: swaps back to the previous software |
| 19 | Reboot | `SQA8` | Empty, then the module restarts |
| 1A | Direct install | `SQA8` | Empty: replaces the software without a trial |

Staging starts from the running image, so Block CRC tells you which
4,096-byte blocks differ and only those need sending. After Commit, Reboot
starts the trial. Identify then reports boot mode 1 and the new build ID;
promote it when it has run well, and Identify reports mode 0. An extension
image for the running build is loaded as soon as it is committed, without
a restart. CRCs detect damage; they are not authentication.

## Developer commands

| Hex | Command | Request | Reply |
| --- | --- | --- | --- |
| 03 | Peek | u32 address, u8 count (1–96) | The bytes. RAM only |
| 04 | Poke | u32 address, 1–44 bytes | Empty. Stock data and heap RAM only |
| 05 | Stream range | u8 index (0–7), u32 address, u8 count (0 clears) | Empty |
| 06 | Stream | u16 period in ms (0 stops) | Empty |

While a stream runs the module sends, each period and unasked, command
`70` (a result byte and the Diagnostics counters) and `71` for each range
(a result byte, the index, the bytes), with sequence 0.

## The working model

Read model and stream kind 0 return the working preset as the module holds
it: 5,728 bytes. Its fields are what Set parameter stores and what the
stock preset file's keys load into. Offsets are hexadecimal.

**Preset**, at 0:

| Offset | Field |
| ---: | --- |
| 00 | Name, 12 bytes, space padded |
| 0C | Data 2 as CV: s16 source, −1 none |
| 0E | MIDI Setup: u8, 0…8 |
| 10, 12, 14, 16 | Crossfade group A–D CV: s16 source, −1 off |
| 18, 1A, 1C, 1E | Crossfade group A–D width: u16, 1…1000 |
| 20 | Eight channels of 2C8 bytes |

**Channel**, at 20 + 2C8 × channel:

| Offset | Field |
| ---: | --- |
| 000 | Eight zones of 4C bytes |
| 260 | Zones CV: s16 source |
| 262 | Zone selection: 0 Gate Rise, 1 Continuous, 2 Random, 3 Advance |
| 263 | Linear AM is external envelope |
| 264 | Channel mode |
| 265 | Reverse |
| 266 | Loop mode |
| 267 | Loop length is end |
| 268 | Crossfade group: 0…3 for A…D |
| 269 | Attack from current |
| 26A | Splice smoothing |
| 26B | Mix mod is fader |
| 26C | Mix level modulation: s16 source, s16 amount |
| 270 | Pitch, s16 |
| 272 | Level, s16 |
| 274 | Pan, s16 |
| 276 | Bits, u16 |
| 278 | Attack, u32 |
| 27C | Release, u32 |
| 280 | Play mode |
| 281 | Auto trigger |
| 282 | Mix level, s16 |
| 284 | Phase mod source |
| 285 | Aliasing |
| 286 | Phase mod index, u16 |
| 288 | Sixteen modulations of four bytes: s16 source, s16 amount, in the order under [Channel](#channel) |

Fields without a type are one byte.

**Zone**, at 4C × zone within its channel:

| Offset | Field |
| ---: | --- |
| 00 | Sample file name, 48 bytes, zero terminated. Empty for an unused zone |
| 30 | u32, for the module's own use: not zero when a sample is attached |
| 34 | Side |
| 36 | Minimum voltage, s16 |
| 38 | Sample start, u32 |
| 3C | Sample end, u32 |
| 40 | Loop start, u32 |
| 44 | Loop length, u32 |
| 48 | Pitch offset, s16 |
| 4A | Level offset, s16 |

A channel's zones in use are the leading ones with a name. The EQ and the
grain settings are not in this model: read them with stream kind 5 and
parameters 170 and 194. [PRESET_FORMAT.md](PRESET_FORMAT.md) describes how
they are kept in the preset file.

## Notes for editor authors

- **Start with Identify.** No reply means stock software or no MIDI
  expander. Check the protocol version and the extension state.
- **Mirror, then follow.** Read the model with stream kind 0, the samples
  with kind 3 and the EQ with kind 5, then poll Preset summary. Its
  checksums tell you which part to read again, and they change for panel
  edits and preset changes as well as your own.
- **Expect Busy.** Loading a folder or saving holds the card for seconds.
  Busy means nothing was done; wait and ask again. Summary's flag bit 1
  says the card is busy before you ask.
- **Trust the stored value.** Set parameter replies what was stored after
  clamping, which may not be what you sent.
- **A value can be changed from the panel** between your read and your
  write. Each Set is a whole value, so the last writer wins.
- **Leave the sequence 0** to automation, and give each request a sequence
  different from your last one so that a late reply is not taken for a new
  one.
