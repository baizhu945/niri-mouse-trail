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
      CFLAGS="$(pkg-config --cflags wayland-client cairo libevdev) -D_GNU_SOURCE -Wall -Wextra -O2 -g -Isrc"
      LIBS="$(pkg-config --libs wayland-client cairo libevdev) -lm"

      gcc $CFLAGS \
        -c src/trail.c -o build/trail.o

      gcc $CFLAGS \
        -c src/wlr-layer-shell-client-protocol.c -o build/wlr-layer-shell.o

      gcc $CFLAGS \
        -c src/xdg-shell-client-protocol.c -o build/xdg-shell.o

      gcc $CFLAGS \
        -c src/relative-pointer-client-protocol.c -o build/rel-ptr.o

      gcc $CFLAGS \
        -c src/main.c -o build/main.o

      gcc build/trail.o build/wlr-layer-shell.o build/xdg-shell.o build/rel-ptr.o build/main.o \
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

  toggle-script = pkgs.writeShellScriptBin "mouse-trail-toggle" ''
    set -euo pipefail
    PIDFILE="/tmp/mouse-trail.pid"
    SOCK="$XDG_RUNTIME_DIR/mouse-trail.sock"

    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
        kill "$(cat "$PIDFILE")" 2>/dev/null || true
        remove-without-permission -f "$PIDFILE" "$SOCK"
        ${pkgs.libnotify}/bin/notify-send "Mouse Trail" "已关闭" 2>/dev/null || true
    else
        remove-without-permission -f "$PIDFILE" "$SOCK"
        ${mouse-trail-pkg}/bin/mouse-trail &
        echo $! > "$PIDFILE"
        sleep 0.2
        ${pkgs.libnotify}/bin/notify-send "Mouse Trail" "已开启" 2>/dev/null || true
    fi
  '';

  ctl-script = pkgs.writeShellScriptBin "mouse-trail-ctl" ''
    set -euo pipefail
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
  ];
}
