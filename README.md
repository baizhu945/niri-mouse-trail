# mouse-trail

A beautiful, meteor-like mouse cursor trail overlay for Wayland compositors (niri, Sway, Hyprland, etc.) built with wlr-layer-shell and Cairo.

[中文](README.zh-CN.md)

![demo](https://img.shields.io/badge/version-0.9-blue)

> ⚠️ **重要警告 / Important Warning**
>
> 本项目使用**环形校准区域**（屏幕中央的 2px 细环）来定期校正光标位置。
> 该区域虽然仅约 1592 px²（不到屏幕的 0.12%），但在**全屏游戏中可能导致屏幕中央的鼠标点击失灵**（如 FPS 射击、MOBA 等需要频繁点击中央区域的游戏）。
>
> **请在进入游戏前运行 `mouse-trail-toggle` 关闭拖尾，游戏结束后再次运行开启。**
>
> This project uses a **ring-shaped calibration region** (a 2px-thin hollow square at screen center) for periodic cursor position correction.
> While only ~1592 px² (<0.12% of screen), it may **block mouse clicks at the screen center in fullscreen games** (FPS, MOBA, etc.).
>
> **Run `mouse-trail-toggle` to disable the trail before gaming, and again to re-enable after.**

---

## Requirements

Before using mouse-trail, ensure your system meets these prerequisites:

- **Wayland compositor** with `wlr-layer-shell-unstable-v1` support (niri, Sway, Hyprland, River, etc.)
- **Input device access**: your user must be in the `input` group to read `/dev/input/event*` devices
  ```bash
  sudo usermod -aG input $USER
  # Log out and back in for the change to take effect
  ```
- **`flat` acceleration profile** for accurate mouse tracking. Set in niri:
  ```kdl
  mouse {
      accel-profile "flat"
  }
  ```
- **evtest** (optional, for debugging input devices): `nix-shell -p evtest`
- **Dependencies for manual compilation**: `wayland`, `wayland-protocols`, `wlroots`, `cairo`, `libevdev`, `pkg-config`, `gcc`

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
mouse-trail-ctl warp                # Trigger full-screen recapture
mouse-trail-ctl help                # Show all commands with defaults
```

> **Monitor-switch tracking is automatic.** The program monitors keyboard events (Super+Shift+Left/Right, Super+Shift+Ctrl+Left/Right) and automatically restarts with full calibration on screen switches. No extra hotkey binding needed.

### CLI options

```
mouse-trail --help

Options:
  --config PATH       Config file (default: ~/.config/mouse-trail/config)
  --device PATH       Input device (overrides auto-detect)
  --kbd-device PATH    Keyboard device (overrides auto-detect)
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
- **Drift correction**: cross-shaped calibration lines provide periodic absolute position ground-truth from the compositor, eliminating integration drift
- **Monitor-switch detection**: keyboard hotkey monitoring (Super+Shift+Left/Right) triggers automatic restart with full recalibration

This is the best achievable solution within Wayland's security constraints — we cannot query the cursor position directly, so we combine evdev tracking with opportunistic compositor calibration.

### Ring Calibration Region

After the initial 5-second calibration window, the input region shrinks to a **hollow ring** (200×200 outer, 196×196 inner, 2px line thickness) at each output's center:

```
    ┌───────────────────────────────────────┐
    │                                       │
    │         ███████████████████████        │
    │         ██                     ██      │
    │         ██                     ██      │
    │         ██     open center     ██      │
    │         ██    (196×196 px)     ██      │
    │         ██   full passthrough  ██      │
    │         ██                     ██      │
    │         ██                     ██      │
    │         ███████████████████████        │
    │        ↑ 200×200 outer, 2px thick ↑   │
    │                                       │
    └───────────────────────────────────────┘
```

Only the 2px-thin hollow square receives pointer events. Everything inside and outside has full click passthrough.

- **Outer dimensions**: 200×200 px
- **Line thickness**: 2 px
- **Inner open area**: 196×196 px (center is completely free)
- **Total active area**: ~1592 px² — less than 0.12% of a 1440×900 surface

**Why a ring?** A closed loop guarantees calibration: to leave the center area in ANY direction, the cursor MUST cross the ring. Unlike a cross pattern, there are no "gaps" to slip through. The center 196×196 area is completely open — UI elements placed at screen center are never blocked.

**Why calibration?** The trail tracks cursor position by integrating velocity from raw evdev events (speed → position). Digital integration inherently accumulates floating-point error over time. When the cursor crosses the ring, `wl_pointer` provides an absolute position ground-truth from the compositor, instantly correcting any accumulated drift.

**Monitor-switch detection** is handled separately by monitoring keyboard hotkeys (Super+Shift+Left/Right). A detected screen switch triggers an automatic restart with full-surface calibration.

### Why 5-second full surface at startup? The initial full-surface window guarantees the cursor position is captured immediately when the trail is first enabled, even if the cursor is stationary. After capture, the bullseye handles subsequent warps.

---

## Architecture

```
┌─ Input Thread (poll() all detected mice/touchpads)
├─ Keyboard Thread (hotkey detection, auto-detected)
                           │
                     trail.pos_x, trail.pos_y
                     trail ring buffer (absolute global coords)
                           │
                           ▼
   Wayland Display ◄── Render Loop (~60Hz, Cairo→wl_shm)
        │
        ▼
   wlr-layer-shell overlay (one surface per wl_output)
   (input_region = bullseye → near-full passthrough)
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
│   └── xdg-shell-client-protocol.c        # Pre-generated xdg-shell (dependency)
├── mouse-trail.nix                        # Nix home-manager module
├── config.example                         # Example configuration
├── Makefile                               # Manual gcc compilation
├── README.md / README.zh-CN.md            # Documentation
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

This can happen when the compositor doesn't send `wl_pointer.enter` at startup (common on niri). The trail initializes at the primary output center. Move the cursor across the screen center (crossing the calibration crosshair) — this will instantly recalibrate the position. Monitor-switch hotkeys (Super+Shift+Left/Right) automatically restart with full calibration.

### Trail lags behind cursor

Ensure your compositor uses `accel-profile "flat"` (no pointer acceleration). The trail tracks raw evdev deltas 1:1.

### No trail visible

1. Check the log: `mouse-trail --log-level debug --log-file /tmp/trail.log`
3. Ensure the compositor supports `wlr-layer-shell-unstable-v1`

### Color cycling looks wrong

Color cycling uses HSL interpolation. The `--cycle-speed` parameter controls how fast the color cycles (default: 5 seconds per full cycle).

---

## License

MIT
