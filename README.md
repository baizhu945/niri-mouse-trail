# mouse-trail

A beautiful, meteor-like mouse cursor trail overlay for Wayland compositors (niri, Sway, Hyprland, etc.) built with wlr-layer-shell and Cairo.

[中文](README.zh-CN.md)

![demo](https://img.shields.io/badge/version-0.9-blue)

---

## About

**mouse-trail** renders a fading, comet-shaped trail behind your mouse cursor on a transparent overlay. It supports per-monitor surfaces, real-time color changes, opacity control, configurable trail width/speed, HSL color cycling, and automatic theme synchronization.

> **This project was developed and debugged primarily by [DeepSeek V4 Pro](https://chat.deepseek.com/), with human testing and verification on NixOS + niri.** Every line of C, every protocol binding, and every bug fix was generated through iterative AI-assisted development. The human contributor performed real-world testing on a dual-monitor HiDPI setup (1.77× / 1.60× fractional scaling), identified edge cases, and guided the debugging process.

---

## Features

- **Meteor-like trail**: head is bright and wide, tail fades cubically and tapers quadratically
- **Multi-monitor**: creates independent layer surfaces for each output
- **Real-time control**: change color, width, opacity, speed via Unix socket
- **HSL color cycling**: continuous rainbow trail with configurable cycle speed
- **Click passthrough**: empty input region — the overlay never blocks mouse clicks
- **Stationary fade**: trail gracefully disappears ~1 second after mouse stops
- **Screen-edge clamping**: trail stays bounded to desktop edges, matching compositor behavior
- **Per-event tracking**: processes evdev events individually for accurate edge behavior
- **Config file**: `~/.config/mouse-trail/config` with `import=` support for external themes
- **Theme sync**: automatically reads noctalia wallpaper colors for global theme consistency
- **Toggle script**: `mouse-trail-toggle` for hotkey binding (run once = ON, run again = OFF)
- **Logging**: timestamped debug/info/warn/error log output for diagnostics

---

## Installation

### NixOS / home-manager

Add the module to your home-manager imports:

```nix
# ~/.config/home-manager/home.nix
{
  imports = [
    ./mouse-trail/mouse-trail.nix
  ];
}
```

Then rebuild:

```bash
home-manager switch
```

This installs three commands: `mouse-trail`, `mouse-trail-toggle`, `mouse-trail-ctl`.

### Manual compilation

```bash
# Dependencies: wayland, wayland-protocols, wlroots, cairo, libevdev, pkg-config, gcc
make
```

The binary will be at `./mouse-trail`.

---

## Usage

### Quick start

```bash
mouse-trail-toggle          # Start (run again to stop)
mouse-trail-ctl help        # List all control commands
```

### Toggle (hotkey binding)

Add to your niri / Sway config:

```
Mod+Ctrl+T { spawn-sh "mouse-trail-toggle"; }
```

### Runtime control

```bash
mouse-trail-ctl color ff6b6b        # Set trail color (hex, with or without #)
mouse-trail-ctl alpha 0.5           # Set opacity (0-1)
mouse-trail-ctl width 12            # Set head radius in pixels
mouse-trail-ctl speed 300           # Set trail duration in ms
mouse-trail-ctl color-cycle on      # Enable HSL rainbow cycling
mouse-trail-ctl color-cycle off     # Disable
mouse-trail-ctl show                # Show trail
mouse-trail-ctl hide                # Hide trail
mouse-trail-ctl help                # Show all commands with defaults
```

### CLI options

```
mouse-trail --help

Options:
  --config PATH       Config file (default: ~/.config/mouse-trail/config)
  --device PATH       Input device (default: /dev/input/event2)
  --color RRGGBB      Trail color (default: ffffff)
  --alpha N           Opacity 0-1 (default: 1.0)
  --width N           Head radius in px (default: 8)
  --length N          Trail duration in ms (default: 500)
  --min-speed N       Stationary threshold in px (default: 2)
  --smooth-factor N   EMA filter 0-1 (default: 0.6)
  --color-cycle on|off
  --cycle-speed N     Cycle period in seconds (default: 5)
  --socket PATH       Control socket path
  --log-level debug|info|warn|error (default: info)
  --log-file PATH     Log file (default: stderr)
  --ctl "CMD"         Send command to running instance and exit
```

### Config file

Default location: `~/.config/mouse-trail/config`

```ini
# Mouse Trail Configuration
# Use import=path to include another config file

color=80c8ff
alpha=1.0
width=8
length=500
min_speed=2
smooth_factor=0.6
color_cycle=off
cycle_speed=5
device=/dev/input/event2
import=/path/to/theme.conf
```

---

## Configuration & Environment Notes

### `remove-without-permission` → `rm`

If you are using this project on a system **other than the original author's NixOS setup**, you need to replace `remove-without-permission` with `rm` in `mouse-trail/mouse-trail.nix` (the toggle script).

**Why:** The original author uses a custom `remove-without-permission` wrapper that bypasses an interactive `rm` wrapper installed system-wide. Most users won't have this wrapper, and `rm` works correctly on standard systems.

To fix, change these lines in `mouse-trail.nix`:

```diff
-        remove-without-permission -f "$PIDFILE" "$SOCK"
+        rm -f "$PIDFILE" "$SOCK"
```

(Appears twice in the toggle script section.)

---

## Design Notes

### Wayland Security vs. Cursor Position Accuracy

Wayland intentionally prevents clients from querying the global cursor position. This is a security feature — a malicious client should not be able to track the user's cursor without their knowledge. However, this creates a fundamental challenge for cursor trail effects, which inherently need to know where the cursor is.

**The trade-off:**

| Method | Accuracy | Click Passthrough | Reliability |
|--------|----------|-------------------|-------------|
| `wl_pointer` (surface-relative) | Perfect | ❌ Blocks clicks | Only on own surface |
| evdev (`/dev/input/event*`) | Good (1:1 with flat accel) | ✅ Full passthrough | Always available |
| `zwp_relative_pointer_manager_v1` | Perfect | ✅ | ❌ Not supported by niri |

**Our hybrid approach:**
- **Continuous tracking**: raw evdev events processed per-event with screen-edge clamping (same behavior as the compositor)
- **Startup calibration**: full-surface `wl_pointer` capture during first 5 seconds of operation
- **Drift correction**: bullseye input region provides periodic ground-truth from the compositor

This is the best achievable solution within Wayland's security constraints — we cannot query the cursor position directly, so we combine evdev tracking with opportunistic compositor calibration.

### Bullseye Input Region

After the initial 5-second calibration window, the input region shrinks to a **bullseye pattern** at each output's center:

```
         │  2×100 vertical arm
         │
    ┌────┼────┐
    │    │    │  ← 100×2 horizontal arm
────┼────┼────┼────  (not to scale)
    │ 10×10 │
────┼─center─┼────
    │       │
    └───────┘
```

- **10×10 center** (100 px²): catches the cursor when it warps to the output center (niri `focus-monitor-*`)
- **Horizontal arm** 100×2 (200 px²): catches cursor moving left/right from center
- **Vertical arm** 2×100 (200 px²): catches cursor moving up/down from center
- **Total area**: ~500 px² — less than 0.04% of a 1440×900 surface

**Why this design?** When niri warps the cursor to another monitor, the cursor lands at the output center. If the cursor is hidden (niri's `hide-after-inactive-ms`), the compositor may delay sending `wl_pointer.enter` events. The bullseye arms catch the cursor as it moves away from center in any direction, providing a second chance at calibration. The minimal area ensures everyday clicks are virtually never blocked.

**Why 5-second full surface at startup?** The initial full-surface window guarantees the cursor position is captured immediately when the trail is first enabled, even if the cursor is stationary. After capture, the bullseye handles subsequent warps.

---

## Architecture

```
/dev/input/event2 ──► Input Thread (libevdev, per-event clamping)
                           │
                     trail.pos_x, trail.pos_y
                     trail ring buffer (absolute global coords)
                           │
                           ▼
   Wayland Display ◄── Render Loop (~60Hz, Cairo→wl_shm)
        │
        ▼
   wlr-layer-shell overlay (one surface per wl_output)
   (input_region = empty → click passthrough)
```

### Key design decisions

- **Absolute global coordinates**: trail points store absolute screen positions, no relative shifting. Eliminates compound floating-point errors.
- **Per-event processing**: each evdev event is processed individually with edge clamping, matching the compositor's cursor behavior at screen boundaries.
- **Fractional scale from geometry**: scale is computed as `physical_pixels / logical_pixels` from output mode and layer surface configure events, avoiding the integer-only `wl_output.scale` limitation.
- **wl_pointer.enter for initial position**: captures absolute cursor position at startup (when available). Falls back to primary output center if the compositor doesn't send the enter event.

### File structure

```
mouse-trail/
├── src/
│   ├── log.h                              # Timestamped logging macros
│   ├── trail.h / trail.c                  # Trail state, ring buffer, cleanup
│   ├── main.c                             # Wayland, Cairo, input, control, CLI
│   ├── wlr-layer-shell-client-protocol.h/c # Pre-generated wlr-layer-shell v1
│   ├── xdg-shell-client-protocol.c        # Pre-generated xdg-shell (dependency)
│   └── relative-pointer-client-protocol.h/c # Generated (unused, kept for reference)
├── mouse-trail.nix                        # Nix home-manager module
├── Makefile                               # Manual gcc compilation
└── test-mouse-trail.sh                    # Log-based verification script
```

---

## Dependencies

| Dependency | Purpose |
|-----------|---------|
| `wayland` | Wayland client protocol |
| `wayland-protocols` | xdg-shell protocol |
| `wlroots` | wlr-layer-shell protocol (XML for code generation) |
| `cairo` | 2D rendering on shared memory buffers |
| `libevdev` | Raw mouse input event reading |

---

## Theme Synchronization

The original author's NixOS configuration includes automatic theme synchronization from [noctalia-shell](https://github.com/Noctalia-Development/noctalia-shell) wallpaper colors:

- `wallpaper-theme-sync.sh` watches `~/.config/noctalia/colors.json`
- On wallpaper change, it pushes the `mPrimary` color to:
  - DankMaterialShell (DMS custom theme)
  - Waybar (CSS `@define-color lyrics`)
  - **mouse-trail** (via `mouse-trail-ctl color`)
- `mouse-trail-toggle` also reads the current noctalia color on startup

---

## Troubleshooting

### Trail appears at wrong position

This happens when the compositor doesn't send `wl_pointer.enter` at startup (common on niri). The trail initializes at the primary output center. Move the cursor to any screen edge — the boundary clamping will snap the trail to the correct position.

### Trail lags behind cursor

Ensure your compositor uses `accel-profile "flat"` (no pointer acceleration). The trail tracks raw evdev deltas 1:1.

### No trail visible

1. Check the input device: `mouse-trail --device /dev/input/event2` (verify with `evtest`)
2. Check logs: `mouse-trail --log-level debug --log-file /tmp/trail.log`
3. Ensure the compositor supports `wlr-layer-shell-unstable-v1`

### Color cycling looks wrong

Color cycling uses HSL interpolation. The `--cycle-speed` parameter controls how fast the color cycles (default: 5 seconds per full cycle).

---

## License

MIT
