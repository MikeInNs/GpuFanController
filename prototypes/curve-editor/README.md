# Offline mouse-enabled curve editor

A small FTXUI prototype to evaluate the terminal interaction **before** replacing
the existing configuration utility. This executable has no daemon/API client,
serial driver, configuration-file writer, or firmware dependency. All values are
examples; nothing here is a measured/calibrated fan profile.

## Build and run

From the project root in Ubuntu / Remote - WSL:

```sh
./scripts/build-curve-prototype.sh
./build/curve-prototype/fanctl-curve-prototype
```

The first configure needs Git/network access to fetch FTXUI v6.1.9, pinned to
commit `5cfed50702f52d51c1b189b5f97f8beaf5eaa2a6`. FTXUI is MIT-licensed;
its source and license are in `build/curve-prototype/_deps/ftxui-src`. Dependencies
are not installed system-wide. Normal host and firmware builds are unchanged.
An existing local checkout can be supplied to CMake with
`-DFETCHCONTENT_SOURCE_DIR_FTXUI=/absolute/path/to/ftxui` for offline setup.

In VS Code, choose **Curve editor prototype (offline, mouse enabled)** in Run
and Debug, then F5 or Ctrl+F5. The pre-launch task builds and tests the prototype.
Click the integrated terminal to interact; do not use the Debug Console.

## Interaction

- Drag one of the graph's four numbered points. The yellow point is selected.
- Click a point's value button below the graph to select it without moving it.
- Edit Temperature and RPM, then click **Update point** or press Enter. Numeric
  entries must be whole numbers; invalid entries do not change the graph.
- On the focused graph, keys **1–4** select a point. Left/Right adjust temperature
  by 1 C; Up/Down adjust target speed by 100 RPM. Tab/Shift+Tab move among controls.
- **Apply demo** validates the fields and copies the draft into local demo memory.
  The grey line is this applied reference; the cyan line is the draft.
- **Reset defaults** restores example points to the draft, not to the applied
  reference. **Quit** / Escape exits; all draft/applied values disappear.

Temperatures stay strictly increasing and RPM never decreases across points.
Dragging is clamped at neighbours/bounds; invalid numeric edits are rejected
instead of silently clamped. Demo bounds are **20–100 C** and **0–14000 RPM**.
These are display/model test bounds, **not** validated safe limits for your fans.
Firmware minimum-duty rules and calibration-based limits are not represented yet.

Minimum viewport: **76×24**; around **100×32 or larger** is more comfortable for
dragging. The curve is drawn using braille subcells, but mouse coordinates are
terminal cells. Exact values below the graph and numeric fields compensate for
the coarse mouse precision at smaller sizes. Resizing cancels an in-progress drag.

## Trying it through SSH

Build on the Ubuntu host you connect to, open an ordinary interactive SSH session,
and run the executable there. No graphical desktop, browser or X11 forwarding is
needed. Your local terminal (and any intervening multiplexer) must forward mouse
events. This prototype does not install or configure an SSH server.

The footer counts **Mouse events** received inside the graph and **Drag moves**.
If these stay at zero when clicking a point, check your terminal/multiplexer mouse
handling. Keyboard editing remains available. Try selecting all four points,
dragging horizontally/vertically, crossing a neighbour, releasing outside the
plot, numeric edits, and resizing the terminal.

Automated tests use a PTY and real SGR mouse input sequences, exercising FTXUI's
actual input parser. They are **not** a substitute for testing your specific SSH
client, terminal settings and connection latency.

## Verification / structure

`build-curve-prototype.sh` runs the model and terminal suites. The model checks
bounds, monotonicity, draft/apply/reset and coordinate mapping. The terminal suite
checks dragging, clicks, exact numeric edits, rejection of invalid values,
keyboard controls, viewport fit and resizing.

- `CurveModel`: values, validation and coordinate conversion, independent of UI.
- `CurveGraph`: graph rendering, hit testing and drag/keyboard interaction.
- `CurveEditor`: fields, buttons, focus, local draft/apply workflow.
- `main`: interactive entry point and read-only QA outputs.

For repeatable layout inspection, use `--snapshot WIDTH HEIGHT` or
`--layout-json WIDTH HEIGHT`. `--report-on-exit` prints the in-memory result to
stdout after leaving the terminal; none of these modes writes a file or config.
