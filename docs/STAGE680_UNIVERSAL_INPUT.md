# Stayplaytion Stage6.8 Universal Input Contract

## Baseline

This work is based on the physically proven Stage6.8.0 Firmware POST boot.
The experimental Stage6.8.1 external-root branch is not part of this line.

## Boot invariant

The computer must reach the Desktop with:

- keyboard + mouse connected;
- keyboard only;
- mouse only;
- neither keyboard nor mouse connected.

Absence of USB HID is not a boot failure and must never enter Rescue Monitor.

## X11 HID lifecycle

Xfbdev owns two logical XInput slots:

- `Evdev keyboard`
- `Evdev mouse`

The slots exist even when no physical event device exists.

When detached:

1. X/LXDE remains alive;
2. KDrive scans `/dev/input/event0..31` every 500 ms;
3. a matching device is attached to the existing logical slot;
4. unplug returns that slot to detached state;
5. replug attaches a new event number without restarting X.

Keyboard matching requires ordinary typing keys to avoid composite media-key interfaces.
Mouse matching requires relative X + Y axes.

## Stayplaytion gamepad lifecycle

The game frontend scans `/dev/input/event0..63` and supports up to four gamepads.
Identification is capability-based rather than VID/PID-specific:

- EV_KEY with game buttons;
- EV_ABS axes or hat;
- optional name hints such as Gamepad, Joystick, Controller or Game Stick.

A controller may be absent at frontend start and connected later.
Disconnect closes only that controller slot; the frontend remains alive and rescans.

### Stayplaytion default controls

| Physical input | Stayplaytion action |
| --- | --- |
| D-pad / left stick Left/Right | Previous/next game or horizontal menu item |
| D-pad / left stick Up/Down | Previous/next system or vertical menu item |
| South / A-style bottom button | OK / launch |
| East / B-style right button | Back / exit current layer |
| Start or North | Service/settings menu |
| L/R shoulder | Previous/next system |

Legacy Linux joystick button codes are accepted as fallbacks.

## RetroArch gamepad lifecycle

The H3531 primary input driver `h3531evdev` owns gamepads directly.
`input_joypad_driver = "null"` is therefore intentional: no second joypad subsystem
is required.

Up to four physical gamepads map to RetroArch ports 1..4.
Keyboard mappings remain available as a port-1 fallback.

### RetroPad mapping

- South -> RetroPad B
- East -> RetroPad A
- West -> RetroPad Y
- North -> RetroPad X
- Select / Start -> Select / Start
- TL/TR -> L/R
- TL2/TR2 -> L2/R2
- thumb-click -> L3/R3
- D-pad buttons or HAT0 -> D-pad
- left/right analog axes -> RetroPad analog sticks

RGUI and emulator cores use the same mapping.

## Safety

These changes do not write SPI, do not call `saveenv`, do not modify U-Boot,
and do not depend on Stage6.8.1.
