{ pkgs, lib, config, ... }:

let
  mouse-trail-pkg = pkgs.stdenv.mkDerivation {
    pname = "mouse-trail";
    version = "0.1.0";

    src = ./.;

    nativeBuildInputs = with pkgs; [ pkg-config ];
    buildInputs = with pkgs; [ wayland cairo libevdev ];

    buildPhase = ''
      mkdir -p build
      CFLAGS="$NIX_CFLAGS_COMPILE $(pkg-config --cflags wayland-client cairo libevdev) -Wall -Wextra -O2 -g -Isrc"
      LIBS="$(pkg-config --libs wayland-client cairo libevdev) -lm"

      gcc $CFLAGS -c src/trail.c -o build/trail.o
      gcc $CFLAGS -c src/wlr-layer-shell-client-protocol.c -o build/wlr-layer-shell.o
      gcc $CFLAGS -c src/xdg-shell-client-protocol.c -o build/xdg-shell.o
      gcc $CFLAGS -c src/main.c -o build/main.o
      gcc build/trail.o build/wlr-layer-shell.o build/xdg-shell.o build/main.o \
        -o mouse-trail $LIBS
    '';

    installPhase = ''
      mkdir -p $out/bin
      cp mouse-trail $out/bin/
    '';

    meta = with lib; {
      description = "Mouse trail overlay effect for Wayland/niri";
      license = licenses.mit;
      platforms = platforms.linux;
      mainProgram = "mouse-trail";
    };
  };

  theme-sync-script = pkgs.writeShellScriptBin "mouse-trail-sync-theme" ''
    set -euo pipefail

    RUNTIME_DIR="''${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
    STATE_DIR="''${XDG_STATE_HOME:-$HOME/.local/state}/noctalia"
    SOCK="$RUNTIME_DIR/mouse-trail.sock"
    COLORS_JSON="$STATE_DIR/wallpaper-colors.json"

    sync_theme() {
      [ -S "$SOCK" ] || return 0
      [ -f "$COLORS_JSON" ] || return 0

      local theme
      theme=$(${pkgs.jq}/bin/jq -r '.mPrimary // empty' "$COLORS_JSON" | sed 's/^#//')
      [ -n "$theme" ] || return 0

      # Socket creation can precede the daemon's accept loop by a few frames.
      # Retry briefly so startup and rapid restarts still receive the color.
      local attempt=0
      while [ "$attempt" -lt 40 ]; do
        if ${mouse-trail-pkg}/bin/mouse-trail --ctl "color $theme" >/dev/null 2>&1; then
          echo "[mouse-trail-theme-sync] synchronized color #$theme"
          return 0
        fi
        attempt=$((attempt + 1))
        sleep 0.05
      done
      return 0
    }

    mkdir -p "$RUNTIME_DIR" "$STATE_DIR"
    sync_theme

    ${pkgs.inotify-tools}/bin/inotifywait -m -q \
      -e create -e moved_to \
      --format '%f' \
      "$RUNTIME_DIR" |
    while IFS= read -r filename; do
      if [ "$filename" = "mouse-trail.sock" ]; then
        sync_theme
      fi
    done
  '';

  toggle-script = pkgs.writeShellScriptBin "mouse-trail-toggle" ''
    set -euo pipefail
    PIDFILE="/tmp/mouse-trail.pid"
    SOCK="$XDG_RUNTIME_DIR/mouse-trail.sock"

    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
        kill "$(cat "$PIDFILE")" 2>/dev/null || true
        remove-without-permission -f "$PIDFILE" "$SOCK"
    else
        remove-without-permission -f "$PIDFILE" "$SOCK"
        ${mouse-trail-pkg}/bin/mouse-trail &
        echo $! > "$PIDFILE"
    fi
  '';

  ctl-script = pkgs.writeShellScriptBin "mouse-trail-ctl" ''
    set -euo pipefail
    if [ "''${1:-}" = "help" ] || [ "$#" -eq 0 ]; then
        echo "Usage: mouse-trail-ctl COMMAND [ARGS]"
        echo ""
        echo "Commands:"
        echo "  color RRGGBB     Set trail color (default: ffffff)"
        echo "  alpha N           Set trail opacity 0-1 (default: 1.0)"
        echo "  width N           Set head radius in px (default: 8)"
        echo "  speed N           Set trail duration in ms (default: 500)"
        echo "  color-cycle on|off  Enable/disable color cycling (default: off)"
        echo "  show / hide       Show or hide trail"
        echo "  warp              Trigger full-screen recapture (for monitor switch)"
        echo "  help              Show this help"
        exit 0
    fi
    SOCK="$XDG_RUNTIME_DIR/mouse-trail.sock"
    if [ ! -S "$SOCK" ]; then
        echo "mouse-trail is not running (socket $SOCK not found)" >&2
        exit 1
    fi
    ${mouse-trail-pkg}/bin/mouse-trail --ctl "$*"
  '';

in
{
  home.packages = [
    mouse-trail-pkg
    toggle-script
    ctl-script
    theme-sync-script
  ];

  home.file = {
    ".config/mouse-trail/config".source = ./config.example;
  };

  systemd.user.services.mouse-trail-theme-sync = {
    Unit = {
      Description = "Keep Noctalia theme color synchronized to mouse-trail";
      After = [ "graphical-session.target" ];
      PartOf = [ "graphical-session.target" ];
    };
    Service = {
      Type = "simple";
      ExecStart = "${theme-sync-script}/bin/mouse-trail-sync-theme";
      Restart = "on-failure";
      RestartSec = "1s";
    };
    Install.WantedBy = [ "graphical-session.target" ];
  };
}
