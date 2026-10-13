#!/usr/bin/env python3
"""Check a built patch against the layout. Given Rossum's software file as
well, apply the patch with patcher.py and, when Node is installed, with the
browser patcher, and require both to agree."""
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from patcher import MAX_DOWNLOAD_BYTES, apply_patch, decode_patch, load_layout, sha256

# Runs web/patcher.mjs as the page does and prints what it made.
NODE_CHECK = """
import {readFileSync} from 'node:fs';
import {patchFirmware, sha256, zipFiles} from './web/patcher.mjs';
const [patch, layout, stock] = process.argv.slice(-3).map(path => readFileSync(path));
const result = await patchFirmware(new Uint8Array(stock), {patch: JSON.parse(patch), layout: JSON.parse(layout)});
zipFiles([{name: 'app', data: result.app}, {name: result.extensionName, data: result.extension}]);
console.log(JSON.stringify({build_id: result.buildId, app: await sha256(result.app), extension: await sha256(result.extension)}));
"""


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    patch_path = Path(sys.argv[1])
    patch, layout = patch_path.read_bytes(), load_layout()
    decoded = decode_patch(patch, layout)
    print(f"{patch_path}: {len(decoded['overlay'])} overlay bytes, {len(decoded['hooks'])} hooks, "
          f"{len(decoded['extension'])} extension bytes")
    if len(sys.argv) == 2:
        print("Set A8_STOCK_APP to Rossum's v3.04 download to apply the patch as well.")
        return
    stock_path = Path(sys.argv[2])
    with stock_path.open("rb") as source:
        stock = source.read(MAX_DOWNLOAD_BYTES + 1)
    app, extension, build_id = apply_patch(stock, patch, layout)
    made = {"build_id": build_id, "app": sha256(app), "extension": sha256(extension)}
    print(f"applied: build {build_id}, app SHA-256 {made['app']}")
    if not shutil.which("node"):
        print("Node is not installed: the browser patcher was not run.")
        return
    output = subprocess.run(["node", "--input-type=module", "-e", NODE_CHECK, str(patch_path.resolve()),
                             str((ROOT / "src/patch_layout.json").resolve()), str(stock_path.resolve())],
                            cwd=ROOT, check=True, capture_output=True, text=True).stdout
    if json.loads(output) != made:
        sys.exit("the browser patcher and patcher.py disagree")
    print("browser patcher: identical files")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        sys.exit(f"check: {error}")
