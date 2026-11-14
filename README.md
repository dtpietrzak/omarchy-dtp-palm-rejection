# omarchy-dtp-palm-rejection

Apple-style palm rejection for MacBook trackpads on Omarchy (Asahi Linux, Hyprland).

Hyprland's `disable_while_typing` is all or nothing: while you type, every new touch is ignored, fingers included. macOS doesn't work that way. It looks at each contact on the trackpad and asks whether it is a fingertip or a palm. A palm resting on the corner gets ignored, and a finger reaching for the pointer still works.

This does the same thing on Linux. Your trackpad already reports each contact's size and shape, but libinput only checks one fixed size limit on it.

## How it works

A small daemon sits between the trackpad and libinput:

- It takes over the built-in trackpad and copies every event to a virtual trackpad with the same name plus ` (palm rejection)`. Hyprland uses that one.
- It judges each contact on its own, from its size, how long and narrow it is, where it landed, how far it has moved, and how long ago you last typed.
- Contacts that look like palms are tagged as palms (`MT_TOOL_PALM`). libinput already ignores tagged contacts, so scrolling, gestures, taps and three-finger drag keep working with the fingers that are left.

The rules:

| Situation | Result |
| --- | --- |
| Very big contact (`palm_size`), any time | Palm until it lifts |
| Lands while typing and is big, or long in an edge zone | Palm until it lifts |
| Lands while typing in an edge zone (left, right or front) | Ignored until typing stops and it moves `edge_release_mm` |
| Lands within `tap_guard_ms` of a key press, in the middle | Ignored until it moves `center_release_mm`, so it can't tap-click but can move the pointer |
| Big or long and resting in an edge zone, typing or not | Palm until it lifts |
| Resting in an edge zone, or big, when a key goes down | Palm until it lifts |
| Has moved more than `rest_mm` | Trusted as a finger |
| Small fingertip in the middle, not just after a key | Finger |

Shortcuts (Ctrl, Alt or Super held), modifier keys, F keys and Escape don't count as typing, so Super+click and similar keep working.

If the daemon stops or crashes, the kernel gives the real trackpad back and it works as stock. The keyboard is read only to know *when* a typing key was pressed; key codes are never stored or logged.

## Requirements

- Omarchy on an Apple Silicon MacBook. It was built on a MacBook Pro 13" M2 (`Apple MTP multi-touch`). Any trackpad that reports touch size (`ABS_MT_TOUCH_MAJOR`) should work, but the defaults are tuned for Apple ones.
- `base-devel` and `libevdev` to build it (`sudo pacman -S --needed base-devel libevdev`).
- Your user in the `wheel` group, which Omarchy sets up, to turn it on and off without sudo.

## Install

```bash
git clone https://github.com/dtpietrzak/omarchy-dtp-palm-rejection.git
cd omarchy-dtp-palm-rejection
./install.sh
```

It builds the daemon, runs the tests, and installs (with sudo):

| File | Purpose |
| --- | --- |
| `/usr/local/bin/omarchy-dtp-palm-rejectiond` | The daemon |
| `/usr/local/bin/omarchy-dtp-palm-rejection` | The on/off and tuning command |
| `/etc/systemd/system/omarchy-dtp-palm-rejection.service` | Starts it at boot and restarts it if it stops |
| `/etc/udev/rules.d/90-omarchy-dtp-palm-rejection.rules` | Marks the virtual trackpad as built in |
| `/etc/omarchy-dtp-palm-rejection/palm-rejection.conf` | Settings, writable by `wheel` |

Then it enables and starts the service. Running it again updates everything and keeps your settings.

Turn off Hyprland's own disable-while-typing, which would still block every new touch while you type. In `~/.config/hypr/input.lua`:

```lua
hl.config({
  input = {
    touchpad = {
      disable_while_typing = false,
    },
  },
})
```

`install.sh` reminds you if it is still on.

## Turn it on or off

```bash
omarchy-dtp-palm-rejection off      # trackpad goes back to stock
omarchy-dtp-palm-rejection on
omarchy-dtp-palm-rejection toggle
omarchy-dtp-palm-rejection status   # on or off, plus the daemon's recent log
```

Changes take effect right away and last across reboots. A switch waits until no finger is on the trackpad, so no touch gets stuck halfway.

## Tune it

The size defaults come from libinput's numbers for Apple trackpads and are a starting point. To see what your own fingers and palms measure:

```bash
omarchy-dtp-palm-rejection monitor
```

Each touch prints a line when its decision changes and when it lifts:

```
lift slot=0 finger reason=- major=620 minor=560 ratio=1.11 ... start=63.2,41.0mm edge=no typing=no travel=14.2mm max_size=640
lift slot=1 palm reason=edge-shape major=1180 minor=640 ratio=1.84 ... start=6.1,8.3mm edge=yes typing=yes ...
```

Tap with a fingertip, rest your palms the way you do while typing, and compare `max_size` (the bigger of the two axes, `major` and `minor`). Set `finger_size` between the two, and `palm_size` below your smallest full palm:

```bash
omarchy-dtp-palm-rejection set finger_size 1100
omarchy-dtp-palm-rejection set side_edge_mm 15
omarchy-dtp-palm-rejection config     # print the current settings
```

`set` checks the value before saving it. You can also edit the settings file directly. Every setting is explained in it.

| Setting | Default | Meaning |
| --- | --- | --- |
| `enabled` | `yes` | Filter palms, or pass the trackpad through as stock |
| `debug` | `no` | Log every touch (what `monitor` turns on) |
| `palm_size` | `1600` | Always a palm above this touch size |
| `finger_size` | `1000` | Bigger is suspicious while typing or in an edge zone |
| `max_ratio` | `1.8` | Longer than this (length / width) is suspicious in an edge zone |
| `typing_ms` | `500` | How long after a key press counts as typing |
| `tap_guard_ms` | `250` | New touches this soon after a key are held back |
| `settle_ms` | `60` | Keep checking a new touch's shape while it lands |
| `side_edge_mm` | `12` | Left and right edge zones |
| `bottom_edge_mm` | `10` | Edge zone along the front, away from the keyboard |
| `edge_release_mm` | `8` | Travel that frees an edge touch once typing stops |
| `center_release_mm` | `1.5` | Travel that frees a tap-guarded touch |
| `rest_mm` | `2` | Less travel than this counts as resting |
| `touchpad_name` | empty | Empty detects the built-in trackpad |
| `keyboard_name` | empty | Empty detects the built-in keyboard |

To see the device names for the last two: `omarchy-dtp-palm-rejection devices`.

## Uninstall

```bash
./uninstall.sh                 # removes everything, settings included
./uninstall.sh --keep-config   # keeps /etc/omarchy-dtp-palm-rejection
```

The trackpad is back to stock as soon as the service stops.

## Troubleshooting

- **Nothing changed:** check `omarchy-dtp-palm-rejection status`. In `hyprctl devices`, the trackpad should be listed twice, the second one ending in `(palm rejection)`.
- **Fingers get ignored:** run `monitor`, check that `reason=` explains it, then raise `finger_size`, or lower `side_edge_mm`, `bottom_edge_mm` or `tap_guard_ms`.
- **The service keeps restarting:** `journalctl -u omarchy-dtp-palm-rejection -n 50`. If it can't find the trackpad, set `touchpad_name` from `omarchy-dtp-palm-rejection devices`.
- **Turn it all off at once:** `sudo systemctl stop omarchy-dtp-palm-rejection`.

## Development

```bash
make          # build/omarchy-dtp-palm-rejectiond
make test     # classifier and CLI tests, no hardware or root needed
sudo build/omarchy-dtp-palm-rejectiond --config config/palm-rejection.conf   # run in the foreground
```

Stop the service before running a copy in the foreground. Only one process can hold the trackpad.

The decision logic is in `src/classifier.c`, with no device code, and its tests in `tests/test_classifier.c`. `src/palm-rejectiond.c` handles the devices, the virtual trackpad and the settings reload.

Once this is proven in daily use, the plan is to propose it upstream (Asahi Linux or libinput, and Omarchy).
