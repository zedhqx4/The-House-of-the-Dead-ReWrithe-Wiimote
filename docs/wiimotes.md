# Native Wii Remote input

ReWrithe's Wiimote backend is independent of SDL gamepads and mouse/lightgun input. Each remote has an independent HID session, IR state, button state and player slot.

## Settings file

The feature is opt-in. Create wiimotes.ini in ReWrithe's settings folder:

    enabled=1
    continuous=1
    p1.smoothing=1
    p2.smoothing=1
    p3.smoothing=1
    p4.smoothing=1

The default bindings are B = fire, A = reload, + = confirm and HOME = start/pause. continuous=1 keeps the backend searching while support is enabled; continuous=0 performs a 20-second search after the backend is asked to scan.

For development, HOTD_WII_REMOTES=1 enables support and requests a scan for that run.

## Platform status

- Windows: the backend uses Windows Bluetooth discovery/authentication and the native HID interface. It targets RVL-CNT-01 and RVL-CNT-01-TR, including DolphinBar Mode 4 HID interfaces.
- Linux: the backend reads the Nintendo HID interface through /dev/hidraw*. Pair the remote in the desktop's Bluetooth settings first; then enable support in wiimotes.ini. The kernel/BlueZ stack owns pairing.
- macOS: the backend reads the Nintendo HID interface through IOKit. Pair the remote in macOS Bluetooth settings first; then enable support in wiimotes.ini.
- Android/Quest: native Wiimote transport is not implemented; normal game inputs are unchanged.

Linux device access also requires the logged-in user to have read/write permission for the matching hidraw node (the distribution's udev/input group policy). A powered infrared sensor bar is required for native pointing. The backend does not emulate a mouse or SDL gamepad. MotionPlus, extension devices and speaker audio are not used.

## Four players

Each connection is assigned to one of P1-P4. The backend reserves a disconnected slot while the device identity remains known, and the game keeps each player's aim and button state separate. The four-player mode must be enabled in the game for P3/P4 gameplay.

## Validation

The protocol decoder, button edge handling, four-point homography calibration and four-player settings have automated tests. Pairing, IR quality and four simultaneous physical remotes still need hardware validation on each operating system.


## Four-point IR calibration

During gameplay, press **F10** to calibrate the first connected Wii Remote. Aim at each of the four on-screen targets (top-left, top-right, bottom-right, bottom-left) and press **+** while both IR dots are visible. The fitted homography is stored in that player's profile in `wiimotes.ini`. Press **Esc** to cancel. Repeat F10 after connecting another remote to calibrate its own player slot. F9 remains the existing light-gun calibration shortcut.
