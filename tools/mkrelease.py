#!/usr/bin/env python3
import pathlib
import re
import subprocess
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent

MODULES = ["qpro_led", "qpro_audiofx"]

# Files whose comments are user-facing documentation, not implementation notes.
KEEP_COMMENTS = set()

SHELL_NAMES = {"update-binary"}
HEREDOC = re.compile(r"<<-?\s*'?([A-Za-z_][A-Za-z0-9_]*)'?")


def is_shell(arc):
    return arc.endswith(".sh") or pathlib.PurePosixPath(arc).name in SHELL_NAMES


def classify(text):
    """Yield (line, is_comment). One state machine, so the stripper and the
    verifier can never disagree about what counts as a comment."""
    heredoc, in_quote = None, False
    for i, line in enumerate(text.splitlines()):
        s = line.strip()

        if heredoc is not None:
            yield line, False
            if s == heredoc:
                heredoc = None
            continue

        if in_quote:
            yield line, False
            if line.count("'") % 2 == 1:
                in_quote = False
            continue

        if i == 0 and line.startswith("#!"):
            yield line, False
            continue

        if s.startswith("#"):
            yield line, True
            continue

        yield line, False

        m = HEREDOC.search(line)
        if m:
            heredoc = m.group(1)
        elif line.count("'") % 2 == 1:
            in_quote = True


def strip_shell(text):
    kept = [l for l, is_comment in classify(text) if not is_comment]
    out, blank = [], False
    for line in kept:
        if not line.strip():
            if blank:
                continue
            blank = True
        else:
            blank = False
        out.append(line)
    while out and not out[-1].strip():
        out.pop()
    return "\n".join(out) + "\n"


def code_lines(text):
    return [l.rstrip() for l, is_comment in classify(text)
            if not is_comment and l.strip()]


def check(name, original, stripped):
    if code_lines(original) != code_lines(stripped):
        raise SystemExit(f"{name}: stripping changed executable lines, refusing")
    p = subprocess.run(["sh", "-n"], input=stripped, text=True, capture_output=True)
    if p.returncode != 0:
        raise SystemExit(f"{name}: stripped script does not parse\n{p.stderr}")


def prop(mod, key):
    for line in (mod / "module" / "module.prop").read_text().splitlines():
        if line.startswith(key + "="):
            return line.split("=", 1)[1].strip()
    raise SystemExit(f"{mod.name}: no {key}= in module.prop")


def build(name, outdir):
    mod = ROOT / name
    src = mod / "module"

    author = prop(mod, "author")
    if author != "Juspertinry":
        raise SystemExit(f"{name}: author is {author!r}, expected 'Juspertinry'")

    out = outdir / f"{name}-{prop(mod, 'version')}.zip"
    outdir.mkdir(parents=True, exist_ok=True)

    TEXT = (".sh", ".prop", ".conf", "update-binary", "updater-script")
    # .gitkeep only exists to keep an empty dir in git, it must not ship
    files = sorted((p, p.relative_to(src).as_posix())
                   for p in src.rglob("*")
                   if p.is_file() and p.name != ".gitkeep")

    stripped_count = 0
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for path, arc in files:
            data = path.read_bytes()
            base = pathlib.PurePosixPath(arc).name
            if arc.endswith(TEXT) or base in TEXT:
                data = data.replace(b"\r\n", b"\n")
            if is_shell(arc) and base not in KEEP_COMMENTS:
                text = data.decode("utf-8")
                new = strip_shell(text)
                check(f"{name}/{arc}", text, new)
                data = new.encode("utf-8")
                stripped_count += 1
            zi = zipfile.ZipInfo(arc)
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = (0o755 if arc.endswith((".sh", "update-binary"))
                                else 0o644) << 16
            z.writestr(zi, data)
    return out, stripped_count


def main():
    outdir = ROOT / "release"
    for name in (sys.argv[1:] or MODULES):
        out, n = build(name, outdir)
        print(f"  {out.name:36} {out.stat().st_size:>7} bytes  "
              f"{n} scripts stripped")


main()
