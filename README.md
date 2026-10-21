# Assimil8or firmware - Tallent Edition

An unofficial firmware extension for the Rossum Electro-Music Assimil8or:

* A three-band EQ and high-pass filter on every channel, with an EQ of its own for any zone and CV control of every band.
* A multimode filter on every channel, or on any zone alone: low-pass, band-pass, high-pass and notch at 12 or 24 dB per octave, band stop and a dual low-pass/band-pass, with resonance and CV control of cutoff, resonance, the type's extra setting and the type.
* Grain mode: a channel or stereo pair plays its sample as up to eight overlapping grains, for stretching, freezing and scattering, with CV control of eight parameters.
* Resample: record a channel's own output straight into one of its zones.
* Presets keep the EQ, filter and grain settings, in files Rossum's software still loads.
* MIDI remote control (with a Locutus): read and edit presets, browse the card, audition and assign samples, drive the panel and read the display. The protocol is documented so anyone can write an editor.

All original features remain. The new pages sit behind a second press of the Mutate and Phase Mod buttons, and Rossum's software can be loaded again at any time.

See the [New Features Guide](https://brandon-tallent.github.io/assimil8or-extended-firmware/manual.html) for how everything works.

## Installing

**[Open the firmware patcher](https://brandon-tallent.github.io/assimil8or-extended-firmware/)**

1. Download **Assimil8or software v3.04** (`app-v304-260610.zip`) from the Downloads section of
   [Rossum's Assimil8or page](https://www.rossum-electro.com/products/assimil8or/). There is no need to unzip it.
2. Open the patcher above and select that zip. `assimil8or-tallent-edition-<version>.zip` downloads automatically.
3. Unzip it and copy both files, `app` and `ext-….bin`, to the top level of your micro SD card.
4. With the card in the front panel slot, use **Utilities > Load Software** and reboot as for any software update.
5. Leave the card in. Shortly after the restart the top line of the display reads `Extension installed`.

Your software file stays in your browser; nothing is uploaded. To use the patcher
offline, click **Save this tool for offline use** on the page, then open the
downloaded HTML file in a current browser.

To go back, load Rossum's own `app` with **Utilities > Load Software**.

## Versions

The patcher shows the version at the top of the page. On the module, go to
**Utilities > About this Module**: the line under `SW Rev` reads
`Tallent Edition Rev v1.0.0`. A version ending in `-dev` is a build between
releases.

[CHANGELOG.md](CHANGELOG.md) lists the changes in each release.
[Releases](https://github.com/brandon-tallent/assimil8or-extended-firmware/releases)
has the patcher for each release.

Each release loads presets saved by earlier releases. A preset saved by a
newer release can lose some settings in an older one. The changelog lists
these cases.

## Writing an editor

[PROTOCOL.md](PROTOCOL.md) is the complete MIDI System Exclusive protocol, with a
runnable example. [PRESET_FORMAT.md](PRESET_FORMAT.md) lists the lines this firmware adds
to preset files.

## Building from source

- Install the [Arm GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
  (`arm-none-eabi-gcc`). The published patcher is built with GCC 16.1.0; another version
  makes a working patch with a build ID of its own.

```sh
make
make check
```

Apply the patch to Rossum's download with both patchers and compare them:

```sh
A8_STOCK_APP=/path/to/app-v304-260610.zip make check
```

Build the patcher page:

```sh
A8_STOCK_APP=/path/to/app-v304-260610.zip make release
```

This writes `dist/assimil8or-patcher.html`, the patcher page with your build
inside it. Open it in a browser and use it as above.

## File layout

- `src/` the overlay: C and assembly that replace a reclaimed region of the stock image
- `ext/` the extension image the module loads from the card: the editor commands and grain mode
- `src/stock_abi.ld` the stock addresses the code links against; `src/patch_layout.json` the instructions the patch replaces
- `tools/make_patch.py` builds the patch; `patcher.py` and `web/` apply it

## License

Our original work is MIT licensed. No rights to Rossum's software are granted by
this repository.
