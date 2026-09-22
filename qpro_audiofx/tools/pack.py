#!/usr/bin/env python3
"""Zip module/ into dist/. Entries are stored with forward slashes and the
scripts keep LF endings, otherwise Magisk's shell chokes on them."""
import pathlib
import sys
import zipfile

root = pathlib.Path(__file__).resolve().parent.parent
mod = root / "module"

version = next(
    line.split("=", 1)[1].strip()
    for line in (mod / "module.prop").read_text().splitlines()
    if line.startswith("version=")
)

out = root / "dist" / f"qpro_audiofx-{version}.zip"
out.parent.mkdir(exist_ok=True)

skip = {".gitkeep"}
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for path in sorted(mod.rglob("*")):
        if not path.is_file() or path.name in skip:
            continue
        arc = path.relative_to(mod).as_posix()
        data = path.read_bytes()
        if b"\r\n" in data:
            sys.exit(f"CRLF in {arc}, refusing to pack")
        z.writestr(arc, data)

print(out)
for info in zipfile.ZipFile(out).infolist():
    print(f"  {info.file_size:7d}  {info.filename}")
