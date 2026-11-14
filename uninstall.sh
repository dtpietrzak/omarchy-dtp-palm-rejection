#!/usr/bin/env bash
# Stop omarchy-dtp-palm-rejection and remove everything install.sh put in
# place. The trackpad goes back to stock as soon as the daemon stops.
#
#   ./uninstall.sh                 remove everything
#   ./uninstall.sh --keep-config   keep /etc/omarchy-dtp-palm-rejection

set -euo pipefail

PREFIX=/usr/local
CONF_DIR=/etc/omarchy-dtp-palm-rejection
SERVICE=omarchy-dtp-palm-rejection.service
RULES=/etc/udev/rules.d/90-omarchy-dtp-palm-rejection.rules
keep_config=false

case ${1:-} in
  "") ;;
  --keep-config) keep_config=true ;;
  -h | --help)
    echo "Usage: uninstall.sh [--keep-config]"
    exit 0
    ;;
  *)
    echo "uninstall: unknown argument: $1" >&2
    exit 1
    ;;
esac

echo "Removing (sudo)..."
sudo systemctl disable --now "$SERVICE" 2>/dev/null || true
sudo rm -f "/etc/systemd/system/$SERVICE" "$RULES" \
  "$PREFIX/bin/omarchy-dtp-palm-rejectiond" "$PREFIX/bin/omarchy-dtp-palm-rejection"
if $keep_config; then
  echo "Kept $CONF_DIR"
else
  sudo rm -rf "$CONF_DIR"
fi
sudo systemctl daemon-reload
sudo udevadm control --reload
echo "Palm rejection removed. The trackpad is back to stock."
