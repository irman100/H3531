# H3531 Game Frontend — Stage 4.0

Stage4.0 is a lightweight TV/game-stick style launcher for the proven H3531 RetroArch runtime.

## Architecture

- `GAMEFRONT.APP` owns `/dev/fb0` and evdev while browsing.
- `es_systems.cfg` is intentionally compatible with the core EmulationStation system model: system name, ROM path, extensions, command, platform and theme.
- The launch command contains the exact libretro core (`-L core.so`). The user never selects cores manually.
- Before launching a game, the frontend releases framebuffer/input ownership. RetroArch owns them while the game is running. After RetroArch exits, the frontend reacquires both devices and redraws.
- `gamelist.xml` in each ROM directory is read for title, description and image metadata.
- PNG/JPEG artwork is decoded with pinned `stb_image`.
- XML is parsed with pinned `tinyxml2`.
- No SDL, OpenGL, Qt or network stack is required on the board.

## Stage4.0 controls

- Left / Right: previous / next game
- Up / Down: previous / next system
- Enter or Space: launch selected game with the system's configured core
- F5 or R: rescan ROM directories
- F1: open the existing RetroArch service/diagnostic menu
- Esc or Backspace: exit frontend

## ROM discovery

Only systems containing at least one matching ROM are displayed.

Default directories:

- NES: `/mnt/usb/games/nes`
- Mega Drive / Genesis: `/mnt/usb/games/md`
- Master System: `/mnt/usb/games/sms`
- Game Gear: `/mnt/usb/games/gg`

## Artwork

Preferred EmulationStation metadata is `<system ROM directory>/gamelist.xml`.

Without a gamelist, Stage4.0 also looks for artwork matching the ROM basename in:

- `media/`
- `images/`
- `boxart/`
- `covers/`
- the ROM directory itself

Supported artwork formats: `.png`, `.jpg`, `.jpeg`.

## Current scope

Stage4.0 intentionally prioritizes a robust automatic launcher and local library UI. Gamepad navigation, favorites/history/search, richer themes and UTF-8 font rendering are follow-up stages after hardware proof of this ownership/launch loop.
