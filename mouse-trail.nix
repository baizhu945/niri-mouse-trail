{ pkgs, lib, config, ... }:

let
  mouse-trail-pkg = pkgs.stdenv.mkDerivation {
    pname = "mouse-trail";
    version = "0.1.0";

    # Keep only build inputs in the store; exclude .git, build/, and local binaries.
    src = lib.cleanSourceWith {
      src = ./.;
      filter = path: type:
        let
          root = toString ./.;
          relative = lib.removePrefix "${root}/" (toString path);
        in
        toString path == root || relative == "src" ||
        (lib.hasPrefix "src/" relative &&
          (lib.hasSuffix ".c" relative || lib.hasSuffix ".h" relative));
    };

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
      [ -S "$SOCK" ] && [ -f "$COLORS_JSON" ] || return 0

      local theme
      theme=$(${pkgs.jq}/bin/jq -r '.mPrimary // empty' "$COLORS_JSON" 2>/dev/null) || return 0
      theme="''${theme#\#}"
      [[ "$theme" =~ ^[[:xdigit:]]{6}$ ]] || return 0

      # Socket creation can precede the daemon's accept loop by a few frames.
      local attempt=0
      while [ "$attempt" -lt 40 ]; do
        if ${mouse-trail-pkg}/bin/mouse-trail --socket "$SOCK" --ctl "color $theme" >/dev/null 2>&1; then
          echo "[mouse-trail-theme-sync] synchronized color #$theme"
          return 0
        fi
        attempt=$((attempt + 1))
        sleep 0.05
      done
      return 0
    }

    [ -d "$RUNTIME_DIR" ] || { echo "Missing XDG runtime directory: $RUNTIME_DIR" >&2; exit 1; }
    mkdir -p "$STATE_DIR"
    sync_theme

    # Keep a continuous watch. A one-shot watch can consume the close_write
    # on a temporary file and miss its immediately following atomic rename.
    # A coprocess avoids an inotifywait | while pipeline hiding watcher failures.
    while true; do
      mkdir -p "$STATE_DIR"
      coproc WATCH {
        exec ${pkgs.inotify-tools}/bin/inotifywait -m -q \
          -e create -e moved_to -e close_write -e delete -e moved_from \
          --format '%w%f' "$RUNTIME_DIR" "$STATE_DIR"
      }
      watch_fd="''${WATCH[0]}"
      watch_write_fd="''${WATCH[1]}"
      watch_pid="$WATCH_PID"
      exec {watch_write_fd}>&-
      sync_theme # close the gap between the initial sync and watch setup
      while true; do
        if IFS= read -r -t 30 event <&"$watch_fd"; then
          case "$event" in
            "$SOCK"|"$COLORS_JSON") sync_theme ;;
          esac
        else
          status=$?
          sync_theme # periodic recovery for missed events / unavailable files
          [ "$status" -eq 142 ] && continue
          break # EOF or watcher failure: restart the coprocess
        fi
      done
      exec {watch_fd}<&-
      wait "$watch_pid" || true
      sleep 1
    done
  '';

  toggle-script = pkgs.writeShellScriptBin "mouse-trail-toggle" ''
    set -euo pipefail
    RUNTIME_DIR="''${XDG_RUNTIME_DIR:?XDG_RUNTIME_DIR must be set}"
    PRIVATE_DIR="$RUNTIME_DIR/mouse-trail"
    PIDFILE="$PRIVATE_DIR/mouse-trail.pid"
    SOCK="$RUNTIME_DIR/mouse-trail.sock"
    BIN="${mouse-trail-pkg}/bin/mouse-trail"
    REMOVE=/run/current-system/sw/bin/remove-without-permission

    [ -d "$RUNTIME_DIR" ] && [ ! -L "$RUNTIME_DIR" ] || { echo "Invalid runtime directory" >&2; exit 1; }
    [ "$(stat -c %u "$RUNTIME_DIR")" = "$(id -u)" ] || { echo "Runtime directory has another owner" >&2; exit 1; }
    umask 077
    if [ ! -e "$PRIVATE_DIR" ] && [ ! -L "$PRIVATE_DIR" ]; then
      mkdir -m 700 "$PRIVATE_DIR"
    fi
    [ -d "$PRIVATE_DIR" ] && [ ! -L "$PRIVATE_DIR" ] &&
      [ "$(stat -c %u:%a "$PRIVATE_DIR")" = "$(id -u):700" ] || {
        echo "Unsafe mouse-trail runtime directory" >&2; exit 1;
      }
    [ ! -L "$PIDFILE" ] && { [ ! -e "$PIDFILE" ] || [ -f "$PIDFILE" ]; } || {
      echo "Unsafe mouse-trail pidfile" >&2; exit 1;
    }
    exec 9>"$PRIVATE_DIR/toggle.lock"
    ${pkgs.util-linux}/bin/flock -x 9

    # /proc start time guards against PID reuse; executable check prevents
    # terminating a different process even if the pidfile was overwritten.
    start_time() {
      local stat_line rest
      local -a fields
      [ -r "/proc/$1/stat" ] || return 1
      stat_line=$(<"/proc/$1/stat")
      rest="''${stat_line##*) }"
      read -r -a fields <<< "$rest"
      [ "''${#fields[@]}" -ge 20 ] || return 1
      printf '%s' "''${fields[19]}"
    }
    is_owned_process() {
      [ -n "''${pid:-}" ] && [ -n "''${birth:-}" ] &&
        [ "$(start_time "$pid" 2>/dev/null)" = "$birth" ] || return 1
      case "$(readlink -f "/proc/$pid/exe" 2>/dev/null)" in
        "$BIN"|/nix/store/*-mouse-trail-*/bin/mouse-trail) return 0 ;;
        *) return 1 ;;
      esac
    }

    if [ -e "$PIDFILE" ]; then
      read -r pid birth < "$PIDFILE" || true
      [[ "''${pid:-}" =~ ^[1-9][0-9]*$ && "''${birth:-}" =~ ^[0-9]+$ ]] || {
        echo "Invalid pidfile; refusing to guess which process to stop" >&2; exit 1;
      }
      if is_owned_process; then
        kill "$pid"
        for ((attempt=0; attempt<50; attempt++)); do
          is_owned_process || break
          sleep 0.1
        done
        if is_owned_process; then
          echo "mouse-trail did not stop; pidfile retained" >&2; exit 1
        fi
        "$REMOVE" -f -- "$PIDFILE"
        exit 0
      fi
      # A dead or reused PID is stale; never signal it.
      "$REMOVE" -f -- "$PIDFILE"
    fi

    # main.c owns socket lifecycle. Never unlink another daemon's socket.
    # It will reject a live socket and clean only an owned stale one.
    if [ -e "$SOCK" ] || [ -L "$SOCK" ]; then
      [ -S "$SOCK" ] && [ ! -L "$SOCK" ] || { echo "Unsafe socket path: $SOCK" >&2; exit 1; }
      if ${pkgs.python3}/bin/python3 -c 'import socket,sys; s=socket.socket(socket.AF_UNIX); s.settimeout(0.5); s.connect(sys.argv[1]); s.close()' "$SOCK" 2>/dev/null; then
        echo "mouse-trail already has a live control socket; refusing duplicate start" >&2
        exit 1
      fi
    fi
    "$BIN" 9>&- &
    pid=$!
    birth=$(start_time "$pid") || { echo "mouse-trail exited during startup" >&2; exit 1; }
    printf '%s %s\n' "$pid" "$birth" > "$PIDFILE"
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
