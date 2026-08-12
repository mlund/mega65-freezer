#!/usr/bin/env python3
"""Photograph the freeze menu in every colour scheme, side by side.

The schemes are sixteen palette entries each, and what they look like together
is not something the source or a screen dump can answer -- colour RAM holds
entry numbers, so a text dump of the screen is identical under all of them.
This drives the emulator once per scheme and joins the frames, which is the only
way to see whether a scheme separates its tiers before it reaches hardware.

One run per scheme because Xemu writes ``-screenshot`` on exit: boot the
freezer, press F1 as many times as it takes to reach the scheme, and let the
harness stop it.  The count comes from SCHEME_BOOT, not from zero -- ``F1``
advances from whichever scheme a tool starts in.

    tools/theme-shots.py --emulator xmega65 --sdimg card.img \\
        --build build/src -o themes.jpg

The card is cloned before the build is copied in, so the image passed in is
never written to.  ``-prg`` opens the freezer with nothing frozen behind it, so
the lower rows are blank; that is the right picture for comparing colours and
the wrong one for showing the menu in use.
"""

import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# The emulator harness lives with the tests, and is the only thing that knows
# how to boot a card clone and press a key at it.  Importing beats a second
# copy that would drift.
sys.path.insert(0, os.path.join(ROOT, "test"))
import card  # noqa: E402
import xemu  # noqa: E402
import xemu_keys  # noqa: E402

# In the order F1 reaches them from SCHEME_BOOT, which is Classic.  Kept here
# rather than read from colours.h because the names are what the pictures are
# called, and a scheme's own enum name is not what a reader wants on a file.
SCHEMES = ["mega65", "workbench", "gruvbox"]


def capture(emulator: str, sdimg: str, build: str, workdir: str) -> list[str]:
    """One PNG per scheme, in SCHEMES order."""
    clone = card.inject(sdimg, workdir, build)
    shots = []

    for index, name in enumerate(SCHEMES):
        out = os.path.join(workdir, f"{index}-{name}.png")

        def drive(sock, presses=index):
            for _ in range(presses):
                xemu_keys.press(sock, xemu_keys.NAMED["f1"])
                # The menu repaints from the palette write, not a redraw, but
                # the key queue still wants the gap.
                time.sleep(0.3)
            xemu_keys.release_keys(sock)
            time.sleep(1.0)

        xemu.launch(
            emulator,
            os.path.join(build, "FREEZER.M65"),
            sdimg=clone,
            # AF_UNIX caps near 104 characters, so this cannot go under a long
            # working directory.
            socket_path=f"/tmp/theme-shot{index}.sock",
            drive=drive,
            extra=["-screenshot", out],
        )
        if not os.path.exists(out):
            raise SystemExit(f"{name}: Xemu wrote no screenshot to {out}")
        shots.append(out)

    return shots


def join(shots: list[str], out: str) -> None:
    """Butt the frames together left to right, with nothing between them."""
    from PIL import Image

    frames = [Image.open(path).convert("RGB") for path in shots]
    joined = Image.new("RGB", (sum(f.width for f in frames), max(f.height for f in frames)))
    at = 0
    for frame in frames:
        joined.paste(frame, (at, 0))
        at += frame.width

    if out.lower().endswith((".jpg", ".jpeg")):
        # 4:4:4 and a high quality: this is pixel art, and chroma subsampling
        # smears every single-pixel colour change in it.
        joined.save(out, "JPEG", quality=95, subsampling=0)
    else:
        joined.save(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--emulator", default="xmega65")
    ap.add_argument("--sdimg", required=True, help="an SD image to clone, never written to")
    ap.add_argument("--build", default="build/src", help="directory holding the .M65 builds")
    ap.add_argument("-o", "--output", default="themes.jpg")
    ap.add_argument("--workdir", default="/tmp/theme-shots", help="where the frames are kept")
    args = ap.parse_args()

    os.makedirs(args.workdir, exist_ok=True)
    shots = capture(args.emulator, args.sdimg, args.build, args.workdir)
    join(shots, args.output)
    print(f"{len(shots)} schemes -> {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
