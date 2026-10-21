#!/usr/bin/env python3
"""Package the patch and the self-contained browser patcher.

The release's patch names the files that applying it must give, so this
needs Rossum's software file once; nothing of it goes into the package."""
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from patcher import MAX_DOWNLOAD_BYTES, apply_patch, decode_patch, load_layout, sha256

PATCH_NAME, PAGE_NAME = "assimil8or.a8patch", "assimil8or-patcher.html"
SCRIPT_MARKER = "<!-- PATCHER_SCRIPT -->"
VERSION_MARKER = "{{VERSION}}"


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: package_release.py ROSSUM_V304_DOWNLOAD")
    layout = load_layout()
    patch = json.loads((ROOT / "build" / PATCH_NAME).read_text())
    patch.pop("result", None)
    with Path(sys.argv[1]).open("rb") as source:
        stock = source.read(MAX_DOWNLOAD_BYTES + 1)
    app, extension, build_id = apply_patch(stock, json.dumps(patch).encode(), layout)
    patch["result"] = {"build_id": build_id, "app_sha256": sha256(app), "extension_sha256": sha256(extension)}
    encoded = (json.dumps(patch, indent=1) + "\n").encode()
    decode_patch(encoded, layout)
    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    (dist / PATCH_NAME).write_bytes(encoded)

    web = ROOT / "web"
    payload = json.dumps({"patch": patch, "layout": layout}, separators=(",", ":")).replace("<", "\\u003c")
    core = re.sub(r"^export ", "", (web / "patcher.mjs").read_text(), flags=re.M)
    script = ('<script id="firmware-data" type="application/json">' + payload + "</script>\n"
              '<script type="module">\n' + core + "\n" + (web / "app.mjs").read_text() + "\n</script>")
    template = (web / "patcher.template.html").read_text()
    if template.count(SCRIPT_MARKER) != 1:
        raise ValueError("browser template must contain exactly one script marker")
    if VERSION_MARKER not in template:
        raise ValueError("browser template must show the version")
    version = (ROOT / "VERSION").read_text().strip()
    html = template.replace(VERSION_MARKER, version).replace(SCRIPT_MARKER, script)
    (ROOT / "patcher.html").write_text(html)
    (dist / PAGE_NAME).write_text(html)
    manifest = {
        "format": 1,
        "firmware": layout["firmware"]["version"],
        "version": version,
        "build_id": build_id,
        "files": {PATCH_NAME: hashlib.sha256(encoded).hexdigest(), PAGE_NAME: hashlib.sha256(html.encode()).hexdigest()},
    }
    (dist / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Packaged version {version}, build {build_id}, in {dist}; no Rossum software included")


if __name__ == "__main__":
    main()
