# SRR Vita

Play **Sonic Runners Revival** on your PlayStation Vita, with touchscreen and physical button controls.

## Installation

1. Install **kubridge.skprx** and enable it under `*KERNEL` in your taiHEN configuration.
2. Install **libshacccg.suprx**, for example using ShaRKBR33D.
3. Download the [VPK from GitHub Releases](https://github.com/ggdrn/SSR-VITA/releases) and install it using VitaShell.
4. Download the [Sonic Runners Revival APK](https://sonicrunners.com/#/download) and copy it to **`ux0:data/srr/srr224.apk`**.
5. Launch the game. For online features, connect to Wi-Fi and check the Vita's date and time.

The game APK is not included. No OBB extraction is required. Required data downloads automatically and is saved locally. If a download fails, close the game and reopen it to retry.

## Port settings

Tap **SETTINGS** in the LiveArea to open the port menu on a black background,
before the game loads. You can also hold **SELECT** when launching the game.
Tap an option's left/right side to change it, or use the D-pad and **X**.
Choose **Start Game** to save and launch with those settings, or **Exit** / **O**
to save and return to the LiveArea. Opening SETTINGS while the game is running
restarts the app into this menu. Low keeps the game's Light quality with half-size
textures to preserve text readability. Material Shaders defaults to On and enables lighting, reflections and material
variants. Set it to Off for simple diffuse rendering if a character looks corrupted.

## Startup shader preparation

Before the game starts, the port prepares shaders already present in its local
cache and displays progress in English. On the first launch there may be no
cache yet; it is created while playing. Preparation uses a bounded memory budget
and leaves uncached or skipped shaders to the game's normal loading path.

## Controls

| Vita input | Action |
|---|---|
| **Cross (X)** | Jump / tap; hold for touch-based character abilities |
| **Square (□)** | Use an item |
| **Triangle (△)** | Switch character |
| **L / Start** | Pause / go back |
| Circle (○) | Alternate jump / tap |
| Select | Alternate back action |
| Front touchscreen | Navigate menus and use the original touch controls |

Item use and character switching are available during races, following the game's normal availability and cooldowns.

## Credits

- **SEGA / Sonic Team** — original Sonic Runners game.
- **Sonic Runners Revival team** — the fan revival and supplied splash artwork.
- **[Rinnegatamante](https://github.com/Rinnegatamante)** and the [vitaGL contributors](https://github.com/Rinnegatamante/vitaGL/graphs/contributors) — vitaGL graphics library.
- **[TheFloW](https://github.com/TheOfficialFloW/kubridge)** — original kubridge; **[bythos14](https://github.com/bythos14/kubridge)** — the extended kubridge variant used by this port.
- **[VitaSDK contributors](https://github.com/vitasdk/vita-toolchain/graphs/contributors)** — SDK and toolchain, including **Princess-of-Sleeping, frangarcj, xyzz, yifanlu, dmchurch, DaveeFTW, d3m3vilurr, isage, 173210, and xerpi**.
- **The PS Vita homebrew community** — shared research, documentation, and support.

Game assets and trademarks belong to their respective owners.

Build instructions and technical notes are in the [development guide](docs/development.md).
