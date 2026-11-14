#!/usr/bin/env bash
# Exercise on/off/toggle/set/config against a scratch copy of the settings.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLI="$ROOT/bin/omarchy-dtp-palm-rejection"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp "$ROOT/config/palm-rejection.conf" "$tmp/conf"
export OMARCHY_DTP_PALM_CONF="$tmp/conf"
export OMARCHY_DTP_PALM_DAEMON="$ROOT/build/omarchy-dtp-palm-rejectiond"
failures=0

check() {
  if ! eval "$2"; then
    echo "FAIL: $1" >&2
    failures=$((failures + 1))
  fi
}
value() { sed -n -E "s/^$1 = (.*)$/\1/p" "$tmp/conf"; }

"$CLI" off >/dev/null 2>&1
check "off sets enabled = no" '[[ $(value enabled) == no ]]'
"$CLI" on >/dev/null 2>&1
check "on sets enabled = yes" '[[ $(value enabled) == yes ]]'
"$CLI" toggle >/dev/null 2>&1
check "toggle turns it off" '[[ $(value enabled) == no ]]'
"$CLI" toggle >/dev/null 2>&1
check "toggle turns it back on" '[[ $(value enabled) == yes ]]'

"$CLI" set finger_size 1234 >/dev/null
check "set changes a value" '[[ $(value finger_size) == 1234 ]]'
check "set leaves comments alone" 'grep -q "^# A touch this big" "$tmp/conf"'

before=$(cat "$tmp/conf")
check "set rejects a bad value" '! "$CLI" set finger_size banana >/dev/null 2>&1'
check "set rejects an unknown key" '! "$CLI" set nope 1 >/dev/null 2>&1'
check "set rejects a bad key name" '! "$CLI" set "a=b" 1 >/dev/null 2>&1'
check "rejected sets leave the file alone" '[[ $(cat "$tmp/conf") == "$before" ]]'

"$CLI" set touchpad_name "Apple MTP multi-touch" >/dev/null
check "set takes names with spaces" '[[ $(value touchpad_name) == "Apple MTP multi-touch" ]]'
check "config lists settings" '"$CLI" config | grep -q "^finger_size = 1234$"'
check "daemon accepts the edited file" '"$OMARCHY_DTP_PALM_DAEMON" --check-config "$tmp/conf" >/dev/null'

chmod 444 "$tmp/conf"
out=$("$CLI" off 2>&1) && status=0 || status=$?
check "unwritable settings fail" '((status == 1))'
check "unwritable settings say why" '[[ $out == *"cannot write"* ]]'
chmod 644 "$tmp/conf"

check "unknown command fails" '! "$CLI" frobnicate >/dev/null 2>&1'
check "help works" '"$CLI" --help | grep -q "Usage"'

if ((failures)); then
  echo "$failures CLI failure(s)" >&2
  exit 1
fi
echo "all CLI tests passed"
