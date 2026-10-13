#!/usr/bin/env python3
"""Apply the Assimil8or patch to a user-supplied Rossum v3.04 software file."""
from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import zipfile
import zlib

ROOT = Path(__file__).resolve().parent
LAYOUT_PATH = ROOT / "src/patch_layout.json"
MAX_PATCH_BYTES = 1_000_000
MAX_DOWNLOAD_BYTES = 4_000_000
APP_HEADER_BYTES = 8  # file bytes, load address
WORD_BYTES = 4
EXTENSION_MAGIC = b"SQA8EXT1"
# ARM branches a hook may be: b, bl and beq, with a 24-bit word offset.
BRANCH_OPCODES = {"b": 0xEA, "bl": 0xEB, "beq": 0x0A}
BRANCH_PIPELINE_BYTES = 8
BRANCH_OFFSET_BITS = 24


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def number(value) -> int:
    """An address or size from the layout or a patch: a hex string or an integer."""
    if type(value) is int:
        return value
    if not isinstance(value, str):
        raise ValueError("invalid number")
    return int(value, 16)


def load_layout() -> dict:
    return json.loads(LAYOUT_PATH.read_text())


def branch_target(address: int, word: bytes, instruction: str) -> int:
    """The address a hook's branch word jumps to; refuses any other instruction."""
    value = struct.unpack("<I", word)[0]
    if value >> 24 != BRANCH_OPCODES[instruction]:
        raise ValueError("hook is not the expected branch")
    offset = value & ((1 << BRANCH_OFFSET_BITS) - 1)
    if offset >> (BRANCH_OFFSET_BITS - 1):
        offset -= 1 << BRANCH_OFFSET_BITS
    return address + BRANCH_PIPELINE_BYTES + offset * WORD_BYTES


def decode_patch(data: bytes, layout: dict) -> dict:
    """Check a patch against the layout: it may only fill the overlay region,
    replace the listed instructions with branches into it, and copy the listed
    stock ranges."""
    if len(data) > MAX_PATCH_BYTES:
        raise ValueError("patch exceeds size limit")
    patch = json.loads(data)
    required = {"format", "base_sha256", "overlay", "hooks", "splices", "extension"}
    if not isinstance(patch, dict) or not required <= set(patch) <= required | {"result"}:
        raise ValueError("invalid patch fields")
    if type(patch["format"]) is not int or patch["format"] != 1:
        raise ValueError("unsupported patch format")
    if patch["base_sha256"] != layout["firmware"]["app_sha256"]:
        raise ValueError("patch targets an unsupported firmware")
    start, end = number(layout["overlay"]["start"]), number(layout["overlay"]["end"])
    overlay = bytes.fromhex(patch["overlay"])
    if not overlay or len(overlay) % WORD_BYTES or len(overlay) > end - start:
        raise ValueError("overlay escapes its region")

    wanted = {number(hook["address"]): hook["instruction"] for hook in layout["hooks"]}
    hooks = {}
    if not isinstance(patch["hooks"], list) or len(patch["hooks"]) != len(wanted):
        raise ValueError("invalid hook count")
    for hook in patch["hooks"]:
        if not isinstance(hook, dict) or set(hook) != {"address", "word"}:
            raise ValueError("invalid hook fields")
        address, word = number(hook["address"]), bytes.fromhex(hook["word"])
        if address not in wanted or address in hooks or len(word) != WORD_BYTES:
            raise ValueError("hook outside the layout")
        if not start <= branch_target(address, word, wanted[address]) < start + len(overlay):
            raise ValueError("hook leaves the overlay")
        hooks[address] = word

    splices = []
    if not isinstance(patch["splices"], list) or len(patch["splices"]) != len(layout["splices"]):
        raise ValueError("invalid splice count")
    for splice, allowed in zip(patch["splices"], layout["splices"]):
        if not isinstance(splice, dict) or set(splice) != {"address", "parts"}:
            raise ValueError("invalid splice fields")
        address, parts = number(splice["address"]), []
        if not isinstance(splice["parts"], list) or len(splice["parts"]) != len(allowed["parts"]):
            raise ValueError("splice differs from the layout")
        for part, shape in zip(splice["parts"], allowed["parts"]):
            if "stock" in shape:
                if part != {"stock": shape["stock"]}:
                    raise ValueError("splice copies an unlisted range")
                parts.append(("stock", number(part["stock"][0]), number(part["stock"][1])))
            else:
                if not isinstance(part, dict) or set(part) != {"data"}:
                    raise ValueError("invalid splice part")
                parts.append(("data", bytes.fromhex(part["data"]), 0))
        size = sum(len(a) if kind == "data" else b for kind, a, b in parts)
        if not start <= address or address + size > start + len(overlay):
            raise ValueError("splice escapes the overlay")
        splices.append((address, parts))

    extension = patch["extension"]
    if not isinstance(extension, dict) or set(extension) != {"image"}:
        raise ValueError("invalid extension fields")
    image = bytes.fromhex(extension["image"])
    fields = layout["extension"]
    if len(image) < number(fields["header_bytes"]) or image[:len(EXTENSION_MAGIC)] != EXTENSION_MAGIC:
        raise ValueError("invalid extension image")
    file_bytes, bss_bytes = struct.unpack_from("<II", image, number(fields["file_bytes_offset"]))
    if file_bytes != len(image) or len(image) + bss_bytes > number(fields["max_bytes"]):
        raise ValueError("extension exceeds its reserved range")
    result = patch.get("result")
    if result is not None and (not isinstance(result, dict) or set(result) != {"build_id", "app_sha256", "extension_sha256"}):
        raise ValueError("invalid result fields")
    return {"overlay": overlay, "hooks": hooks, "splices": splices, "extension": image, "result": result}


def read_app(data: bytes, layout: dict) -> bytes:
    """The stock `app`, from Rossum's zip or on its own."""
    if data[:2] == b"PK":
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            names = [name for name in archive.namelist() if not name.endswith("/")]
            if names != ["app"] or archive.getinfo("app").file_size > MAX_DOWNLOAD_BYTES:
                raise ValueError("the zip is not Rossum's Assimil8or software download")
            data = archive.read("app")
    if sha256(data) != layout["firmware"]["app_sha256"]:
        raise ValueError(f"input must be the unmodified Rossum Assimil8or v{layout['firmware']['version']} `app`")
    return data


def apply_patch(app: bytes, patch_data: bytes, layout: dict | None = None) -> tuple[bytes, bytes, str]:
    """Returns the patched `app`, its extension image and the build ID."""
    layout = layout or load_layout()
    patch = decode_patch(patch_data, layout)
    stock = read_app(app, layout)
    base = number(layout["firmware"]["load_address"]) - APP_HEADER_BYTES  # address of file offset 0
    start, end = number(layout["overlay"]["start"]), number(layout["overlay"]["end"])
    image = bytearray(stock)
    for address, word in patch["hooks"].items():
        image[address - base:address - base + WORD_BYTES] = word
    image[start - base:end - base] = patch["overlay"] + bytes(end - start - len(patch["overlay"]))
    for address, parts in patch["splices"]:
        at = address - base
        for kind, a, b in parts:
            chunk = a if kind == "data" else stock[a - base:a - base + b]
            image[at:at + len(chunk)] = chunk
            at += len(chunk)
    # The identity record: the build ID is the image's hash with the record's
    # ID and CRC zero, and the CRC covers the image with only itself zero.
    payload = memoryview(image)[APP_HEADER_BYTES:]
    build_id_at = start - base + number(layout["overlay"]["build_id_offset"])
    crc_at = start - base + number(layout["overlay"]["crc_offset"])
    build_id = hashlib.sha256(payload).digest()[:8]
    image[build_id_at:build_id_at + len(build_id)] = build_id
    struct.pack_into("<I", image, crc_at, zlib.crc32(payload))
    # The extension names the build it links against and carries its own CRC.
    fields = layout["extension"]
    extension = bytearray(patch["extension"])
    at = number(fields["build_id_offset"])
    extension[at:at + len(build_id)] = build_id
    struct.pack_into("<I", extension, number(fields["crc_offset"]), zlib.crc32(extension))
    result = patch["result"]
    if result and (result["build_id"] != build_id.hex() or result["app_sha256"] != sha256(image) or
                   result["extension_sha256"] != sha256(extension)):
        raise ValueError("patched files differ from the release's")
    return bytes(image), bytes(extension), build_id.hex()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware", type=Path, help="Rossum's v3.04 zip or its `app` file, downloaded separately")
    parser.add_argument("patch", type=Path, help="the .a8patch file")
    parser.add_argument("-o", "--output", type=Path, required=True,
                        help="folder for the two files to copy to the card; existing files are never overwritten")
    args = parser.parse_args()
    try:
        layout = load_layout()
        with args.patch.open("rb") as source:
            patch_data = source.read(MAX_PATCH_BYTES + 1)
        with args.firmware.open("rb") as source:
            firmware = source.read(MAX_DOWNLOAD_BYTES + 1)
        app, extension, build_id = apply_patch(firmware, patch_data, layout)
        args.output.mkdir(parents=True, exist_ok=True)
        names = ("app", layout["extension"]["name"].replace("{build_id}", build_id))
        for name, data in zip(names, (app, extension)):
            with (args.output / name).open("xb") as output:
                output.write(data)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.error(str(error))
    print(f"Created {args.output / names[0]} and {args.output / names[1]}\nBuild {build_id}; app SHA-256 {sha256(app)}")
    print("Copy both to the root of the card, then use Utilities > Load Software. Keep Rossum's download for going back.")


if __name__ == "__main__":
    main()
