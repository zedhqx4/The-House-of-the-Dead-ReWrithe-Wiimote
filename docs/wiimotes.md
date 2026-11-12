# Native Wii Remote input

ReWrithe's Wiimote backend is independent of SDL gamepads and mouse/lightgun input. Each remote has an independent HID session, IR state, button state and player slot.

## Settings file

The feature is opt-in. Create wiimotes.ini in ReWrithe's settings folder:

    enabled=1
    continuous=1
    rumble=1
    p1.smoothing=1
    p2.smoothing=1
    p3.smoothing=1
    p4.smoothing=1
    p1.buttons=4 8 2 4096

The global `rumble` setting defaults to on; set `rumble=0` or turn **Rumble** off in Options > Controls > Wii Remotes to disable it. Vibration follows the game's existing feedback events: taking damage (300 ms), hitting an enemy (70 ms), and a registered reload (90 ms). It does not vibrate merely because Fire or Reload was pressed. The Wii Remote has an on/off motor, so gamepad intensity levels are not available.

The four values on each pN.buttons line are the gameplay actions Fire, Reload, Confirm and Start / Pause, in that order. The default is B = fire, A = reload, 1 = the additional Confirm binding and + = Start / Pause. + exits the boot attract to the menu, joins / continues while the player is waiting, and opens / closes the in-game pause menu. A always accepts the selected item in the pause menu; B is not used as its accept button. The player page in Options > Controls > Wii Remotes lets you select an action and press the new Wii Remote button. Each physical button can be assigned to only one of the four actions for that player. continuous=1 keeps the backend searching while support is enabled; continuous=0 performs a 20-second search after the backend is asked to scan.

For development, HOTD_WII_REMOTES=1 enables support and requests a scan for that run.

## Platform status

- Windows: the backend uses Windows Bluetooth discovery/authentication and the native HID interface. It targets RVL-CNT-01 and RVL-CNT-01-TR, including DolphinBar Mode 4 HID interfaces.
- Linux: the backend reads the Nintendo HID interface through /dev/hidraw*. Pair the remote in the desktop's Bluetooth settings first; then enable support in wiimotes.ini. The kernel/BlueZ stack owns pairing.
- macOS: the backend reads the Nintendo HID interface through IOKit. Pair the remote in macOS Bluetooth settings first; then enable support in wiimotes.ini.
- Android/Quest: native Wiimote transport is not implemented; normal game inputs are unchanged.

Linux device access also requires the logged-in user to have read/write permission for the matching hidraw node (the distribution's udev/input group policy). A powered infrared sensor bar is required for native pointing. In the front-end menus, the IR aim moves ReWrithe's own hand cursor; **A** clicks the focused item under that cursor and **B** goes back. Menu navigation is separate from the remappable gameplay actions (**B** fire, **A** reload, **1** Confirm and **+** Start / Pause by default). This does not move or emulate the operating-system mouse. MotionPlus, extension devices and speaker audio are not used.

## Four players

Each connection is assigned to one of P1-P4. The backend reserves a disconnected slot while the device identity remains known, and the game keeps each player's aim and button state separate. The four-player mode must be enabled in the game for P3/P4 gameplay.

## Developer notes

The implementation is split into three layers:

- `wiimote_protocol.cpp` is platform-independent. It owns report decoding, button-edge detection, memory-write report construction, settings validation and the four-point projective calibration.
- `wiimote.cpp` adapts decoded snapshots to ReWrithe's existing light-gun, pause-menu and front-end input paths. It also turns game-state changes (damage, score increases and registered reloads) into timed rumble pulses.
- `wiimote_win.cpp`, `wiimote_linux.cpp` and `wiimote_mac.cpp` own the native HID transport and worker threads. Linux/macOS use the operating system's existing Bluetooth pairing; Windows additionally performs native discovery/authentication. The unsupported-platform backend keeps common game code buildable without a transport.

`Remote::buttons` is the current held-button mask; `Remote::pressed` is a rising-edge mask. A backend poll consumes its copy of the edge mask, while the game-facing adapter preserves pending edges until the frame input path calls `ClearEdges()`. `connected` describes a live HID session; `reserved` describes ownership of a player slot, which can survive a temporary disconnect so the same remote can reclaim its seat.

IR calibration maps four ordered camera samples (top-left, top-right, bottom-right, bottom-left) to the corresponding screen targets with a 3x3 homography. The fit rejects crossed/degenerate sample quads and singular solutions. All three native transports request extended report `0x33`, allowing the same decoder to process button and IR data on every platform.

## Validation

The protocol decoder, button edge handling, four-point homography calibration and four-player settings have automated tests. Pairing, IR quality and four simultaneous physical remotes still need hardware validation on each operating system.


## Setup and four-point IR calibration

Open **Options > Controls > Wii Remotes**. Turn **Support** on, choose **Scan for Remotes**, then open the desired player slot. The page shows connection, IR tracking, battery and aim-calibration status. Linux and macOS users should pair the remote in the operating system's Bluetooth settings first.

Choose **Calibrate Aim** from that player's page. The four targets appear over the menu (top-left, top-right, bottom-right, bottom-left). Aim at each target and press **A** while both IR dots are visible. Press **B** or **Esc** to cancel without replacing the previous calibration. The fitted homography is saved automatically to that player's profile in `wiimotes.ini`. Repeat separately for each connected player.
