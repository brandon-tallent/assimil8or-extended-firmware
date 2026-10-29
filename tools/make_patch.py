#!/usr/bin/env python3
"""Compile and link the overlay and the extension, and package them as a patch.

No Rossum software is an input: the code links against the addresses in
src/stock_abi.ld and the patch names the instructions it replaces
(src/patch_layout.json). patcher.py applies it to the user's own copy."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import eq_coefficients
from patcher import BRANCH_OFFSET_BITS, BRANCH_OPCODES, BRANCH_PIPELINE_BYTES, WORD_BYTES, decode_patch, number

# The release this tree builds: MAJOR.MINOR.PATCH, with -dev between releases.
VERSION_FILE = ROOT / "VERSION"
VERSION_PATTERN = r"(\d+)\.(\d+)\.(\d+)(-dev)?"


def version() -> tuple[int, int, int, bool]:
    """(major, minor, patch, development build) from VERSION."""
    match = re.fullmatch(VERSION_PATTERN, VERSION_FILE.read_text().strip())
    if not match or any(int(part) > 255 for part in match.groups()[:3]):
        raise ValueError("VERSION must be MAJOR.MINOR.PATCH, each 0-255, optionally ending -dev")
    return int(match[1]), int(match[2]), int(match[3]), bool(match[4])


def version_flags() -> list[str]:
    major, minor, patch, dev = version()
    return [f"-DPATCH_VERSION_MAJOR={major}", f"-DPATCH_VERSION_MINOR={minor}",
            f"-DPATCH_VERSION_PATCH={patch}", f"-DPATCH_VERSION_DEV={int(dev)}"]


# Sources that use the VFP in the foreground; everything else must leave it alone.
# NEON sources also use NEON, built for speed (the audio interrupt saves
# every NEON register).
FLOAT_SOURCES = ("eq_math", "eq_model", "eq_panel")
NEON_SOURCES = ("hits", "joins")
IRQ_SOURCES = ("timelock",)  # the audio interrupt's work, built for speed
SPLICE_INSERT_SECTION = ".splice.{name}"


def run(*args: str) -> str:
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def symbols(prefix: str, elf: Path) -> dict[str, int]:
    rows = run(prefix + "nm", "--defined-only", str(elf))
    return {name: int(address, 16) for address, _, name in
            (row.split() for row in rows.splitlines() if len(row.split()) == 3)
            if re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", name)}


def undefined(prefix: str, objects: list[str]) -> set[str]:
    rows = run(prefix + "nm", "--undefined-only", *objects)
    return {row.split()[-1] for row in rows.splitlines() if row.strip().startswith("U ")}


def branch(address: int, target: int, instruction: str) -> bytes:
    delta = target - address - BRANCH_PIPELINE_BYTES
    if delta % WORD_BYTES or not -(1 << (BRANCH_OFFSET_BITS + 1)) <= delta < (1 << (BRANCH_OFFSET_BITS + 1)):
        raise ValueError("branch target outside ARM range")
    return struct.pack("<I", BRANCH_OPCODES[instruction] << 24 | (delta // WORD_BYTES & ((1 << BRANCH_OFFSET_BITS) - 1)))


def compile_source(prefix: str, source: Path, build: Path, include: Path) -> str:
    obj = build / (source.stem + ".o")
    fp_flags = (["-mfloat-abi=softfp", "-mfpu=neon"] if source.stem in NEON_SOURCES else
                ["-mfloat-abi=softfp", "-mfpu=vfpv3"] if source.stem in FLOAT_SOURCES else
                ["-mfloat-abi=soft", "-mgeneral-regs-only"])
    optimise = (["-O3", "-funroll-loops"] if source.stem in NEON_SOURCES else
                ["-O2"] if source.stem in IRQ_SOURCES else ["-Os"])
    subprocess.run([prefix + "gcc", "-c", "-std=c11", *optimise, "-g",
                    "-mcpu=cortex-a8", "-marm", *fp_flags,
                    "-ffreestanding", "-fno-builtin", "-fno-common", "-fno-unwind-tables",
                    "-fno-asynchronous-unwind-tables", "-Wall", "-Wextra", "-Werror", *version_flags(),
                    f"-I{include}", str(source), "-o", str(obj)], check=True)
    return str(obj)


def compile_all(prefix: str, overlay_sources: Path, extension_sources: Path, build: Path) -> tuple[list[str], list[str]]:
    """Objects of the overlay and of the extension."""
    build.mkdir(parents=True, exist_ok=True)
    eq_source = build / "eq_fixed.S"
    eq_source.write_text(eq_coefficients.assembly(eq_coefficients.neutral_configuration()))
    overlay = [compile_source(prefix, source, build, overlay_sources) for source in
               [*sorted(overlay_sources.glob("*")), eq_source] if source.suffix in (".c", ".S")]
    ext_build = build / "ext"
    ext_build.mkdir(exist_ok=True)
    extension = [compile_source(prefix, source, ext_build, overlay_sources)
                 for source in sorted(extension_sources.glob("*.c"))]
    return overlay, extension


def make_patch(prefix: str, overlay_sources: Path, extension_sources: Path, link: Path, abi: Path,
               layout_path: Path, build: Path) -> dict:
    """Build everything under `build` and return the patch as a dictionary."""
    layout = json.loads(layout_path.read_text())
    start, end = number(layout["overlay"]["start"]), number(layout["overlay"]["end"])
    overlay_objects, extension_objects = compile_all(prefix, overlay_sources, extension_sources, build)
    search = ["-L", str(abi.parent), "-L", str(build)]
    elf = build / "patch.elf"
    subprocess.run([prefix + "ld", "--no-warn-rwx-segments", *search, "-T", str(link / "patch.ld"),
                    "-Map=" + str(build / "patch.map"), "-o", str(elf), *overlay_objects], check=True)
    subprocess.run([prefix + "objcopy", "-O", "binary", "--only-section=.patch", str(elf), str(build / "patch.bin")], check=True)
    names = symbols(prefix, elf)
    overlay = (build / "patch.bin").read_bytes()
    if not overlay or start + len(overlay) > end:
        raise ValueError("invalid overlay size")

    hooks = [{"address": hook["address"],
              "word": branch(number(hook["address"]), names[hook["target"]], hook["instruction"]).hex()}
             for hook in layout["hooks"]]
    # A splice fills space the overlay reserves with ranges of the stock
    # image, copied when the patch is applied, around blocks of our own.
    splices = []
    for splice in layout["splices"]:
        parts, size = [], 0
        for part in splice["parts"]:
            if "stock" in part:
                parts.append({"stock": part["stock"]})
                size += number(part["stock"][1])
            else:
                section = SPLICE_INSERT_SECTION.format(name=part["insert"])
                block = build / (part["insert"] + ".bin")
                subprocess.run([prefix + "objcopy", "-O", "binary", "--only-section=" + section, str(elf), str(block)], check=True)
                parts.append({"data": block.read_bytes().hex()})
                size += len(block.read_bytes())
        if size != names[splice["end"]] - names[splice["target"]]:
            raise ValueError(f"splice {splice['target']} does not fill its space")
        splices.append({"address": f"0x{names[splice['target']]:08x}", "parts": parts})

    # The extension links against the stock addresses and the overlay's own
    # symbols. Names both define (memcpy and friends) resolve to stock.
    stock_names = set(re.findall(r"PROVIDE\((\w+) =", abi.read_text()))
    storage_start, storage_end = number(layout["storage"]["start"]), number(layout["storage"]["end"])
    (build / "overlay.ld").write_text("".join(
        f"PROVIDE({name} = 0x{address:x});\n" for name, address in sorted(names.items())
        if (start <= address < end or storage_start <= address < storage_end) and name not in stock_names))
    extension_elf = build / "extension.elf"
    subprocess.run([prefix + "ld", "--no-warn-rwx-segments", *search, "-T", str(link / "ext.ld"),
                    "-Map=" + str(build / "extension.map"), "-o", str(extension_elf), *extension_objects], check=True)
    raw = build / "extension.raw"
    subprocess.run([prefix + "objcopy", "-O", "binary", "--only-section=.extension", str(extension_elf), str(raw)], check=True)
    fields = layout["extension"]
    image = bytearray(raw.read_bytes())
    bss_bytes = next(int(row.split()[1]) for row in run(prefix + "size", "-A", str(extension_elf)).splitlines()
                     if row.startswith(".bss"))
    if symbols(prefix, extension_elf)["extension_header"] != number(fields["base"]):
        raise ValueError("extension header moved")
    struct.pack_into("<II", image, number(fields["file_bytes_offset"]), len(image), bss_bytes)

    patch = {"format": 1, "base_sha256": layout["firmware"]["app_sha256"], "overlay": overlay.hex(),
             "hooks": hooks, "splices": splices, "extension": {"image": image.hex()}}
    decode_patch(json.dumps(patch).encode(), layout)
    patch["_summary"] = (f"{len(overlay)}/{end - start} overlay bytes, "
                         f"{names['patch_storage_end'] - storage_start}/{storage_end - storage_start} storage; "
                         f"{len(hooks)} hooks; extension {len(image)} + {bss_bytes} BSS bytes")
    return patch


def write_patch(patch: dict, output: Path) -> None:
    output.write_text(json.dumps({key: value for key, value in patch.items() if not key.startswith("_")}, indent=1) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--prefix", default="arm-none-eabi-")
    parser.add_argument("--overlay", type=Path, default=ROOT / "src")
    parser.add_argument("--extension", type=Path, default=ROOT / "ext")
    parser.add_argument("--link", type=Path, default=ROOT / "src", help="folder of patch.ld and ext.ld")
    parser.add_argument("--abi", type=Path, default=ROOT / "src/stock_abi.ld")
    parser.add_argument("--layout", type=Path, default=ROOT / "src/patch_layout.json")
    parser.add_argument("--build", type=Path, default=ROOT / "build")
    args = parser.parse_args()
    patch = make_patch(args.prefix, args.overlay, args.extension, args.link, args.abi, args.layout, args.build)
    write_patch(patch, args.output)
    print(f"{args.output}: {patch['_summary']}")


if __name__ == "__main__":
    main()
