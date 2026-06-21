# Mouse Trail Effect — Design Spec

## Overview
A declarative, reproducible mouse trail effect for niri (Wayland). A transparent overlay window renders a meteor-like trailing shape behind the cursor, driven by relative input events from `/dev/input/event2`. Controllable via a toggle script and runtime commands.

## Requirements

1. Configurable width, trail speed/length, color, smoothing
2. Real-time color change (command-based + optional auto-cycle)
3. Click passthrough — overlay must not intercept mouse input
4. Trail disappears when mouse is stationary (below speed threshold)
5. Toggle script (run once → on, run again → off) for hotkey binding
6. Motion filtering to smooth hand tremors
7. Meteor-like shape: head is bright and wide, tail fades and narrows

## Architecture

```
/dev/input/event2 ──► Input Thread (libevdev)
                           │
                     [EMA filter] ──► filtered (x,y)
                           │
                           ▼
                     Trail Ring Buffer
                     [(x,y,ts), ...]       ◄── Control Socket (UDS)
                           │                        ▲
                           ▼                        │
   Wayland Display ◄── Render Loop          mouse-trail-ctl
        │              (Cairo→wl_shm)
        ▼
   wlr-layer-shell overlay
   (input_region = empty → passthrough)
```

## Components

### 1. `mouse-trail` — C binary

| File | Purpose |
|------|---------|
| `main.c` | CLI arg parsing, daemon entry, signal handling |
| `wayland.c/h` | Wayland connection, wlr-layer-shell, wl_shm pool, Cairo surface, render loop |
| `trail.c/h` | Trail ring buffer (positions + timestamps), EMA filter, age/fade calculation |
| `log.h` | Timestamped logging macros (DEBUG/INFO/WARN/ERROR) |

**Dependencies:** `wayland`, `wayland-protocols`, `wlroots` (for wlr-layer-shell), `cairo`, `libevdev`

**Render approach:** Cairo draws on a `wl_shm`-backed surface. Each frame:
1. Clear buffer (all transparent)
2. Iterate trail points youngest→oldest
3. For point at age `a` (ms), compute:
   - `t = min(a / max_age, 1.0)` (normalized age 0..1)
   - `alpha = 1.0 - t³` (fast fade at end)
   - `radius = max_radius × (1.0 - t)²` (quadratic taper)
4. Draw filled circle at point position with BGRA color × alpha
5. `wl_surface_commit`

**EMA Filter:**
```
filtered_x = prev_x × α + raw_x × (1 − α)
filtered_y = prev_y × α + raw_y × (1 − α)
```
where `α` ∈ [0, 1] (default 0.6). Higher = smoother.

**Stationary detection:** If no movement above `min_speed` pixels for `idle_ms` (≈ 2× trail length), trigger fade-out. No render when stationary.

### 2. `mouse-trail-toggle` — Shell script
```bash
PIDFILE="/tmp/mouse-trail.pid"
SOCK="$XDG_RUNTIME_DIR/mouse-trail.sock"

if kill -0 "$(cat $PIDFILE)" 2>/dev/null; then
    kill "$(cat $PIDFILE)"
    rm -f "$PIDFILE" "$SOCK"
else
    rm -f "$PIDFILE" "$SOCK"
    mouse-trail &
    echo $! > "$PIDFILE"
fi
```

### 3. `mouse-trail-ctl` — Shell script for runtime control
```bash
# Sends commands via UDS
mouse-trail-ctl color "#ff0000"     # set color
mouse-trail-ctl color-cycle on      # enable auto-cycle
mouse-trail-ctl color-cycle off     # disable auto-cycle
mouse-trail-ctl width 12            # set trail width
mouse-trail-ctl speed 300           # set trail length (ms)
```

### 4. `mouse-trail.nix` — Nix home-manager module
```nix
{ pkgs, ... }: {
  home.packages = [
    (pkgs.writeShellScriptBin "mouse-trail-toggle" ''...'')
    (pkgs.writeShellScriptBin "mouse-trail-ctl" ''...'')
    mouse-trail-pkg  # compiled from src/
  ];
}
```

## Configurable Parameters

| CLI flag | Default | Description |
|----------|---------|-------------|
| `--device` | `/dev/input/event2` | Input device path |
| `--color` | `#ffffff` | Trail color (hex RGBA) |
| `--width` | `8` | Max trail head radius (px) |
| `--length` | `500` | Trail duration (ms) |
| `--min-speed` | `2` | Speed threshold for stationary (px/frame) |
| `--smooth-factor` | `0.6` | EMA filter strength (0-1) |
| `--color-cycle` | `off` | Auto color cycling |
| `--cycle-speed` | `5` | Cycle period (seconds) |
| `--socket` | `$XDG_RUNTIME_DIR/mouse-trail.sock` | UDS path |
| `--log-level` | `info` | debug/info/warn/error |
| `--log-file` | `stderr` | Log output path |

## Click Passthrough

```c
struct wl_region *empty = wl_compositor_create_region(compositor);
wl_surface_set_input_region(surface, empty);
wl_region_destroy(empty);
```

An empty input region on the wl_surface ensures all mouse events pass through to windows below.

## Log Format

```
[2026-06-22 10:30:45.123] [INFO ] main.c:42     Starting mouse-trail v0.1.0
[2026-06-22 10:30:45.124] [INFO ] input.c:23     Opened /dev/input/event2 (libevdev)
[2026-06-22 10:30:45.125] [INFO ] wayland.c:67    Bound wlr_layer_shell v1
[2026-06-22 10:30:45.126] [INFO ] wayland.c:89    Layer surface created: 2560x1600
[2026-06-22 10:30:46.001] [DEBUG] trail.c:56      Trail: pts=3 head=(1204,800) age=0ms
[2026-06-22 10:30:48.500] [INFO ] trail.c:89      Mouse stationary, fading out
```

## Verification Plan

1. **Build test:** `nix-build` the derivation, confirm binary exists
2. **Log test:** Run with `--log-level debug --log-file /tmp/trail.log`, move mouse, inspect log
3. **Overlay test:** Run under niri, verify transparent overlay appears
4. **Click test:** With trail active, click windows/buttons below → must work
5. **Trail test:** Move mouse rapidly, verify fading meteor-like trail visible
6. **Stationary test:** Stop moving, verify trail fades within length × 2 ms
7. **Toggle test:** Run toggle twice, verify start/stop
8. **Color test:** `mouse-trail-ctl color "#ff0000"`, verify real-time change
9. **Filter test:** With `--smooth-factor 0.95`, verify very smooth trails; with `0.1`, verify responsive but jittery
