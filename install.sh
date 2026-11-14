#!/usr/bin/env bash
# Build and install omarchy-dtp-palm-rejection, then start it. The system
# parts (binaries, systemd service, udev rule, settings file) need sudo.
# Safe to run again: it rebuilds, reinstalls, restarts the daemon and keeps
# your settings.
#
#   ./install.sh

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
PREFIX=/usr/local
CONF_DIR=/etc/omarchy-dtp-palm-rejection
CONF="$CONF_DIR/palm-rejection.conf"
SERVICE=omarchy-dtp-palm-rejection.service
RULES=/etc/udev/rules.d/90-omarchy-dtp-palm-rejection.rules

fail() {
  echo "install: $*" >&2
  exit 1
}

if [[ ${1:-} == "-h" || ${1:-} == "--help" ]]; then
  echo "Usage: install.sh"
  echo "Builds the daemon, installs it with a systemd service, and turns palm rejection on."
  exit 0
fi
[[ $# -eq 0 ]] || fail "unknown argument: $1"
[[ $EUID -ne 0 ]] || fail "run as your user; it asks for sudo when it needs it"

for tool in cc make pkg-config; do
  command -v "$tool" >/dev/null || fail "$tool is missing: sudo pacman -S --needed base-devel"
done
pkg-config --exists libevdev || fail "libevdev is missing: sudo pacman -S --needed libevdev"

echo "Building..."
make -C "$ROOT" --no-print-directory
"$ROOT/build/test_classifier"

echo "Installing (sudo)..."
sudo install -Dm755 "$ROOT/build/omarchy-dtp-palm-rejectiond" "$PREFIX/bin/omarchy-dtp-palm-rejectiond"
sudo install -Dm755 "$ROOT/bin/omarchy-dtp-palm-rejection" "$PREFIX/bin/omarchy-dtp-palm-rejection"
sudo install -Dm644 "$ROOT/system/$SERVICE" "/etc/systemd/system/$SERVICE"
sudo install -Dm644 "$ROOT/system/90-omarchy-dtp-palm-rejection.rules" "$RULES"
sudo install -d -m755 "$CONF_DIR"
if sudo test -f "$CONF"; then
  echo "Keeping your settings in $CONF"
else
  sudo install -m664 -o root -g wheel "$ROOT/config/palm-rejection.conf" "$CONF"
fi

sudo udevadm control --reload
sudo systemctl daemon-reload
sudo systemctl enable "$SERVICE" >/dev/null 2>&1
sudo systemctl restart "$SERVICE"

sleep 1
if systemctl is-active --quiet "$SERVICE"; then
  echo "Palm rejection is running. Turn it off with: omarchy-dtp-palm-rejection off"
else
  echo "The daemon did not start. Its log:" >&2
  journalctl -u "$SERVICE" -n 20 -o cat --no-pager >&2 || true
  exit 1
fi

if ! id -nG | tr ' ' '\n' | grep -qx wheel; then
  echo
  echo "Note: you are not in the wheel group, so 'omarchy-dtp-palm-rejection on/off'"
  echo "needs sudo. Add yourself with: sudo usermod -aG wheel $USER"
fi

if command -v hyprctl >/dev/null &&
  hyprctl getoption input:touchpad:disable_while_typing -j 2>/dev/null | grep -q '"bool": true'; then
  echo
  echo "Note: Hyprland's disable_while_typing is on. It still blocks every new touch"
  echo "while you type, which is what this replaces. Turn it off in"
  echo "~/.config/hypr/input.lua:"
  echo
  echo "  hl.config({ input = { touchpad = { disable_while_typing = false } } })"
fi
