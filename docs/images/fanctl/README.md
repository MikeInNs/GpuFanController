# FanCtl manual screenshots

These PNGs show FanCtl 1.7.0 rendered at 120x36 against **synthetic HTTP fixtures**.
They are not real hardware measurements or recommended settings. No installed
daemon, serial port, GPU or PC speaker is used. The simulated fault is deliberate.

The capture script runs the real compiled terminal application through the same
PTY/mouse harness as the UI integration tests. It records ANSI output and asserts
the expected pages/dialogs. Rendering decodes that output with pyte, checks every
character against the harness's final screen, then draws the cells and their
colors using Pillow. Only the clearly labeled example footer is added outside
the application. The renderer draws Unicode Braille dot patterns directly for
the curve, avoiding missing glyphs in common terminal fonts.

## Regenerate

From the repository root on Linux, with host binaries already built:

```sh
python3 scripts/capture-manual.py capture \
  --binary build/release/fanctl --output build/manual-capture
```

For rendering, use an isolated Python environment with Pillow and pyte installed
(the committed captures used Pillow 12.3.0 and pyte 0.8.2), and a Unicode monospace
font such as DejaVu Sans Mono:

```sh
python3 scripts/capture-manual.py render \
  --source build/manual-capture --output docs/images/fanctl \
  --font /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
```

Rendering can also run on Windows: copy the capture directory across and use an
appropriate font path. These optional documentation dependencies are not runtime
dependencies of FanCtl. Inspect every image before committing, particularly the
graph, longer dialogs and calibration table. Update the fixture/version caption
when UI behavior changes. Keep raw `.ansi` and `.txt` captures under `build/`, not
in the published documentation. Do not replace simulated capture with a live
hardware session just to refresh screenshots.
