# Daggerfall Unity — Nintendo Switch port (Unity 2022.3.62f3, IL2CPP)
 
This is a Switch port of **Daggerfall Unity**, built by wrapping the Android
ARM64 build in a native loader — the same lineage as the PvZ Fusion, Clone Hero
and Killer Bean ports. It contains **no game assets**. You supply the Android
APK's contents and Daggerfall's own data from your own copy.
 
## Install & run
 
You need two things: the Daggerfall Unity Android APK (`dfu_il2cpp-64bit-v1.1.1.9_mods-not-supported.apk`), and
`ARENA2` from Daggerfall, which Bethesda gives away free.
 
Put the `.nro` in **any** folder on the card and unpack the rest next to it. The
loader finds its own folder at runtime, so the name and location are up to you:
 
```
sdmc:/switch/daggerfall_nx
├── daggerfall_nx.nro
├── libmain.so  libunity.so  libil2cpp.so  lib_burst_generated.so
│                                          <- from the APK's lib/arm64-v8a/
├── assets/                                <- the APK's assets/ folder, ENTIRE
│   ├── bin/                                  ~326 files, 74 MB
│   ├── aa/  Text/  Fonts/  Quests/  Tables/  BIOGs/  SpellIcons/ ...
└── arena2/                                <- Daggerfall's game data, 1680 files
```
 
Launch from hbmenu, or by **title override** (hold R while starting an installed
game) for the full heap — Daggerfall wants it.
 
 ## Controls
 
| Input | Action |
|---|---|
| Left stick | Move (D-pad is menus, not movement) |
| Right stick | Look |
| B / A / X / Y | Jump / Activate / Cast spell / Crouch |
| L / R | Sneak / Ready weapon |
| ZR | Swing weapon |
| Stick click L / R | Run / Centre view |
| D-pad | Inventory / Character sheet / Travel map / Automap |
| + / – | Escape / Status |
| **ZL held** | **Modifier — switches the whole pad to a second layer** |
| ZL + B / A / X / Y | Rest / Transport / Use magic item / Switch hand |
| ZL + D-pad | Steal / Grab / Info / Talk mode |
| ZL + L / R | Logbook / Notebook |
| ZL + ＋ / – | Quicksave / Quickload |
| ZL + stick click R | Console |
 
Everything above goes through Daggerfall's own keybindings, so rebinding in-game
works and the pad follows.
 
### Virtual cursor
 
| Input | Action |
|---|---|
| **ZL + ZR** | Show / hide the cursor |
| Left stick | Move it |
| A / ZR / ZL | Click |
| – | Toggle gyro pointing |
 
The cursor is on by default when docked and off in handheld. While it is up the
left stick steers it instead of walking, and A clicks instead of activating. The
touchscreen is always live in handheld. A USB mouse works in both modes and
turns gyro off while connected. Sensitivities live in `pointer.cfg`.
 
### Text entry
 
Naming a character opens the Switch keyboard automatically. **Y** reopens it if
you cancel.
 
## Building
 
Requires devkitPro with `switch-dev`, plus:
 
```
pacman -S switch-sdl2 switch-mesa switch-libdrm_nouveau switch-libpng switch-zlib
```
 
Then:
 
```
make
```

## Credits
 
Daggerfall Unity is by **Interkarma** and the Daggerfall Workshop team. The
original Daggerfall is Bethesda's, released as freeware.
 
The loader is the open-source Switch homebrew lineage — **TheOfficialFloW**'s
Vita/Switch tradition by way of **Andy Nguyen**, **fgsfds** and
**ChanseyIsTheBest** — reaching this project through the PvZ Fusion port, with
the JNI layer from **clonehero_nx**, the interned-string pool from
**phigros_nx**, JNI hardening from **killerbean_nx**, and the import surface
from **Bouncemasters**. All MIT-licensed. Thanks to everyone in that lineage.
 
Icon art from the Daggerfall Unity cover.
