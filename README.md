# PC Game Controller Fixes

Small, open-source fixes for controller problems in older PC games. Each fix is a
DLL and a small settings file that you drop into the game folder. All of a fix's
code is in one readable C++ source file, with no third-party code.

| Game | Problem | Fix |
|---|---|---|
| [Tom Clancy's Splinter Cell: Conviction](#splinter-cell-conviction) | Xbox One / Series controllers have scrambled buttons: B does A's action, X does B's | `dinput8.dll` tells the game your controller is an Xbox 360 controller |
| [Tom Clancy's Ghost Recon: Future Soldier](#ghost-recon-future-soldier) | No option to invert vertical look on a controller | `xinput1_3.dll` inverts the right stick's up/down |

**Contents**

- [TL;DR](#tldr)
- [Is this safe?](#is-this-safe)
- [Splinter Cell: Conviction](#splinter-cell-conviction)
- [Ghost Recon: Future Soldier](#ghost-recon-future-soldier)
- [How the problems were found](#how-the-problems-were-found)
- [Building from source](#building-from-source)
- [FAQ](#faq)
- [Reporting problems](#reporting-problems)
- [License](#license)

## TL;DR

Setup takes about two minutes. You only need the steps for the game you're fixing.

### What each fix includes

| | Needed to **run** (what you download) | Needed to **build** (only if you build it yourself) |
|---|---|---|
| Splinter Cell: Conviction | `dinput8.dll` + `dinput8_proxy.ini` | `dinput8_proxy.cpp` + `dinput8.def` + `build.bat` |
| Ghost Recon: Future Soldier | `xinput1_3.dll` + `xinput_proxy.ini` | `xinput_proxy.cpp` + `xinput1_3.def` + `build.bat` |

### Splinter Cell: Conviction (scrambled buttons)

1. Go to the [Releases page](https://github.com/G-Lazer/PCGameControllerFixes/releases/latest). Under **Assets**, click
   **SplinterCellConviction-dinput8.zip** to download it.
2. Open your Downloads folder, right-click the zip file and choose **Extract All**,
   then click **Extract**. A folder opens with two files in it: **dinput8** and
   **dinput8_proxy**.
3. Open **Steam**, right-click **Splinter Cell: Conviction** in your library, and
   choose **Manage → Browse local files**. The game's folder opens.
4. In the game's folder, double-click the **src** folder, then the **system** folder.
5. Copy both files from step 2 into this **system** folder. If Windows asks for
   permission, click **Continue**.
6. Start the game, go to the options and make sure **Use controller** is turned on.

That's it. You don't need to change anything else.

### Ghost Recon: Future Soldier (invert look)

1. Go to the [Releases page](https://github.com/G-Lazer/PCGameControllerFixes/releases/latest). Under **Assets**, click
   **GhostReconFutureSoldier-xinput1_3.zip** to download it.
2. Open your Downloads folder, right-click the zip file and choose **Extract All**,
   then click **Extract**. A folder opens with two files in it: **xinput1_3** and
   **xinput_proxy**.
3. Open **Steam**, right-click **Ghost Recon: Future Soldier** in your library, and
   choose **Manage → Browse local files**. The game's folder opens.
4. Copy both files from step 2 straight into this folder (not into any folder inside
   it). If Windows asks for permission, click **Continue**.
5. Back in Steam, right-click the game again and choose **Properties**. Click
   **Controller**, then change the dropdown to **Disable Steam Input**.
6. Start the game. Pushing the right stick up now looks down.

That's it. The Steam overlay can stay on.

### Good to know

- **Windows or your antivirus may warn about the files.** That's common for small
  game fixes like these: they aren't signed by a well-known publisher, and they
  change how a game works, which antivirus programs watch for. See
  [Is this safe?](#is-this-safe) if you want to check them first.
- **To undo a fix,** open the same folder again and delete the two files you
  copied in (plus the log file the fix creates there).
- **Playing Future Soldier online?** Read the [multiplayer warning](#ghost-recon-future-soldier) first.

---

## Is this safe?

You should absolutely be suspicious about downloading custom DLL files like these
and using them without a second thought. I've included the source files for each
below, so feel free to verify their safety and build them yourself if you have any
concerns.

- **Small and readable.** All of each fix's code is in one file:
  [`dinput8_proxy.cpp`](splinter-cell-conviction/src/dinput8_proxy.cpp) and
  [`xinput_proxy.cpp`](ghost-recon-future-soldier/src/xinput_proxy.cpp). Next to
  each is a short `.def` file listing the functions the DLL provides (used when
  building), a `build.bat`, and the `.ini` settings file that ships with the DLL.
- **They pass everything through to Windows.** Each DLL loads the real Windows DLL
  only by its full `System32` path, so nothing else can be substituted for it, and
  changes only what's described below.
- **No network, no registry, no other programs.** The only file either DLL writes
  is its own log, next to itself.
- **Release downloads are built by GitHub, not on someone's PC.** The DLLs on the
  [Releases](https://github.com/G-Lazer/PCGameControllerFixes/releases/latest) page are compiled from this source by
  [GitHub Actions](.github/workflows/build.yml), with a signed build attestation
  you can check (see [Verifying a download](#verifying-a-download)).

The DLLs are not code-signed, so Windows SmartScreen or antivirus software may
warn about them. The Future Soldier fix in particular rewrites how the game calls
XInput (explained [below](#ghost-recon-future-soldier-1)), which antivirus
heuristics can find suspicious. If you'd rather not trust a download at all,
[build it yourself](#building-from-source).

### Verifying a download

Each release lists the SHA256 fingerprint of every file. In PowerShell:

```powershell
Get-FileHash .\dinput8.dll
```

To confirm a file was built by this repository's GitHub workflow from this exact
source (needs the [GitHub CLI](https://cli.github.com/)):

```powershell
gh attestation verify .\dinput8.dll --repo G-Lazer/PCGameControllerFixes
```

---

## Splinter Cell: Conviction

### Symptoms

With an Xbox One or Xbox Series controller, the face buttons do the wrong things.
For example B does what A should, and X does what B should. The on-screen prompts
show Xbox buttons, but pressing the button shown doesn't do that action.

### Install

1. Download `SplinterCellConviction-dinput8.zip` from [Releases](https://github.com/G-Lazer/PCGameControllerFixes/releases/latest).
2. Copy `dinput8.dll` and `dinput8_proxy.ini` into the game's **`src\system`**
   folder, next to `conviction_game.exe`. For the Steam version this is usually:
   ```
   C:\Program Files (x86)\Steam\steamapps\common\Tom Clancy's Splinter Cell Conviction\src\system
   ```
   (In Steam: right-click the game → **Manage → Browse local files**, then open `src\system`.)
3. Start the game and make sure **Use controller** is turned on in the options.

Keep `dinput8_proxy.ini` next to the DLL. The Xbox 360 ID it reports to the game
is set in that file, so without it the DLL changes nothing.

### Settings (`dinput8_proxy.ini`)

| Setting | Default | What it does |
|---|---|---|
| `[Spoof] VidPid` | `045E:028E` | The USB ID reported to the game for every controller. `045E:028E` is the wired Xbox 360 controller, the only one Conviction recognizes. Leave empty to report the real ID. |
| `[Spoof] ProductName` | `Controller (XBOX 360 For Windows)` | The controller name reported to the game. Leave empty to keep the real name. |
| `[Remap] ButtonN=M` | *(none)* | When the game reads button N, give it physical button M. Numbers start at 1; `0` disables a button. For unusual controllers; not needed for Xbox pads. |
| `[Devices] Hide` | *(empty)* | Comma-separated parts of device names to hide from the game, e.g. `Logitech,Wheel`. |
| `[General] Log` | `1` | Write `dinput8_proxy.log` next to the DLL (a few lines per launch, overwritten each time). |
| `[General] LogInput` | `0` | Also log every button press and large stick movement. Useful for troubleshooting only. |

### Uninstall

Delete `dinput8.dll`, `dinput8_proxy.ini` and `dinput8_proxy.log` from `src\system`.

### Things that *don't* fix this (so you can skip them)

- Editing the `[KT_LOGITECH]` / `[KT_SAITEK]` sections of `ProfileDefaultsPC.ini`.
  The most common advice online is to delete them. Making them identical to the
  Xbox 360 section, which should have the same effect, changed nothing on the
  tested setup.
- Deleting your profile save. Note that Ubisoft Connect's cloud sync quietly
  restores the old save unless you turn cloud saves off first.

---

## Ghost Recon: Future Soldier

### Symptoms

The game's invert-look option only affects the mouse. There is no way to invert
vertical look on a controller.

### Install

1. Download `GhostReconFutureSoldier-xinput1_3.zip` from [Releases](https://github.com/G-Lazer/PCGameControllerFixes/releases/latest).
2. Copy `xinput1_3.dll` and `xinput_proxy.ini` into the game's main folder, next to
   `Future Soldier DX11.exe` and `Future Soldier DX9.exe`. For the Steam version this is usually:
   ```
   C:\Program Files (x86)\Steam\steamapps\common\Tom Clancy's Ghost Recon Future Soldier
   ```
3. **Turn Steam Input off for this game:** right-click the game in Steam →
   **Properties → Controller → Disable Steam Input**. On the tested setup, Steam's
   default layout for this game turned the controller into a keyboard and mouse,
   which bypasses this fix and breaks normal controller support.

> ⚠️ **Multiplayer:** Future Soldier uses the PunkBuster anti-cheat. A replacement
> `xinput1_3.dll` that redirects the game's controller calls may be flagged in
> PunkBuster-protected multiplayer. Remove the DLL before playing online if you're
> worried. The campaign and offline play are unaffected.

### Settings (`xinput_proxy.ini`)

| Setting | Default | What it does |
|---|---|---|
| `[Invert] RightY` | `1` | Invert the right stick's up/down (vertical look). |
| `[Invert] RightX` | `0` | Invert the right stick's left/right. |
| `[Invert] LeftY` / `LeftX` | `0` | Invert the left stick (movement). |
| `[Compatibility] RouteAroundHooks` | `1` | Point the game's XInput calls straight at this DLL's own code so Steam's in-game hook can't bypass it. **Without this the fix doesn't work under Steam** (see [why](#ghost-recon-future-soldier-1)). |
| `[General] Log` | `1` | Write `xinput_proxy.log` next to the DLL (a few lines per launch). |
| `[General] LogStats` | `0` | Also log how often the game reads the controller, plus, for two minutes after launch, where its XInput calls actually go. Useful for troubleshooting only. |

### Uninstall

Delete `xinput1_3.dll`, `xinput_proxy.ini` and `xinput_proxy.log` from the game folder.

---

## How the problems were found

Neither fix was the first idea. Both bugs turned out to be different from what the
usual online advice assumes, so here is how each was tracked down, in case it helps
anyone fixing other games.

### Splinter Cell: Conviction

**1. The usual fix didn't work.** `src\system\ProfileDefaultsPC.ini` contains
button translation tables for three controller types: `[KT_X360]` (each button
maps to itself) and `[KT_LOGITECH]` / `[KT_SAITEK]` (buttons shuffled for those
brands' old gamepads). The Logitech table explains the symptom exactly: it turns
button 2 into A and button 3 into B. The common advice is to delete those
sections. Making them identical to the Xbox 360 table changed nothing. Resetting
the player profile, so the game would rebuild it from the edited file, didn't help either.

**2. The tables weren't hiding anywhere obvious.** Searching the game's executable
and DLLs for the Logitech table, as 8-, 16- and 32-bit values, found nothing.
Whatever copy of the layout the game applies, it isn't that ini file.

**3. A logging DirectInput DLL showed what the game actually does.** Rather than
keep guessing, the first version of this DLL simply passed everything through to
Windows and logged it. The log showed the game:

- uses DirectInput (not XInput) for the controller,
- opens the controller, reads its USB vendor/product ID (`DIPROP_VIDPID`), closes
  it, then opens it again for real,
- receives button numbers in Windows' standard order (A = 1, B = 2, X = 3).

The ID is the key. Windows reports Xbox One and Series controllers to DirectInput as
`045E:02FF`. A wired Xbox 360 controller is `045E:028E`, and that is the only ID
this 2010 game knows. Anything else is treated as a generic gamepad and gets the
Logitech layout: button 2 (B) becomes A, button 3 (X) becomes B.

**4. The fix:** report `045E:028E` (and the Xbox 360 name) to the game wherever it
can see the ID: the `DIPROP_VIDPID` property, the device list and the device info.
The game then uses its Xbox 360 layout and every button is correct.

### Ghost Recon: Future Soldier

**1. Steam Input didn't work.** Turning on Steam Input with "Invert Vertical Axis"
broke controller support entirely. Steam's saved layout for this game turned out to
be its *Keyboard (WASD) and Mouse* template.

**2. The game reads the controller through XInput.** The executable imports
`XINPUT1_3.dll` by number: ordinal 2 (`XInputGetState`), 3 (`XInputSetState`) and
4 (`XInputGetCapabilities`). A logging DirectInput DLL confirmed the game only uses
DirectInput for the keyboard and mouse. So the fix should be a replacement
`xinput1_3.dll` that flips the right stick's Y value.

**3. That worked in testing but did nothing in the game.** A test program confirmed
the DLL flipped the stick and nothing else. In the game, the look stayed normal.

**4. The game stopped calling the DLL after startup.** With call counting turned
on, the log showed only **two** controller reads reaching the DLL, both at launch.
After that the game kept working with the controller, but through something else.

**5. Steam was patching the DLL.** The DLL was changed to inspect itself from inside
the game. The game's import table still pointed at the DLL, but the first bytes of
the DLL's `XInputGetState`, `XInputSetState` and `XInputGetCapabilities` had been
overwritten with a jump. That jump led to a small block of allocated memory and
from there into **`gameoverlayrenderer.dll`**, Steam's in-game component. Steam
loads it into the game even with Steam Input disabled and the overlay switched off,
and its replacement code reads the controller itself instead of calling the
original function. The patch was already in place before the game's first
controller read.

**6. The fix:** the DLL's real work lives in private functions that aren't
exported, so nothing outside knows they exist. At startup, the DLL points the
game's XInput imports straight at those private copies and rechecks periodically
in case anything changes them back. Steam's patch stays in place but is never
reached. The test program `hooktest.exe` reproduces Steam's patch exactly and
confirms the inverted stick still gets through.

### Takeaways for other games

- **Log before you remap.** Both fixes looked like simple remapping problems. The
  real causes were a controller-ID check and an overlay hook, and logging found
  them in one run each.
- **Old games often check the controller's USB ID.** If an old game mishandles a
  newer controller, find out which ID it expects.
- **Overlays patch input functions,** including those in replacement DLLs. If a
  proxy DLL's changes don't show up in game, check whether calls are reaching it at all.

---

## Building from source

**Requirements:** Windows, and [Visual Studio 2022](https://visualstudio.microsoft.com/)
(the free Community edition is fine) with the **Desktop development with C++** workload.

```bat
cd splinter-cell-conviction
build.bat
```

```bat
cd ghost-recon-future-soldier
build.bat
```

The DLLs land in each project's `build\` folder. Both are 32-bit (both games are
32-bit) and statically linked, so they need no Visual C++ runtime installed.

### Test tools

Built alongside each DLL. Both need an Xbox-compatible controller connected.

| Tool | What it checks |
|---|---|
| `splinter-cell-conviction\build\probe.exe [seconds]` | Loads the DLL like a game would, lists the controllers DirectInput sees (as the game will see them), and prints buttons pressed during the given number of seconds. |
| `ghost-recon-future-soldier\build\probe.exe [seconds]` | Reads the controller through the DLL and through Windows' real XInput side by side, and checks only the right stick's Y axis differs. |
| `ghost-recon-future-soldier\build\hooktest.exe` | Patches the DLL the way Steam's overlay does, then checks the game-style calls still come back inverted. |

### Repository layout

```
PCGameControllerFixes/
├─ README.md, LICENSE, SECURITY.md
├─ .github/workflows/build.yml    builds the release DLLs on GitHub
├─ splinter-cell-conviction/      dinput8.dll fix
│  ├─ src/dinput8_proxy.cpp       all of the fix's code
│  ├─ src/dinput8.def             exported functions
│  ├─ tools/probe.cpp             test tool
│  ├─ dinput8_proxy.ini           default settings
│  └─ build.bat
└─ ghost-recon-future-soldier/    xinput1_3.dll fix
   ├─ src/xinput_proxy.cpp        all of the fix's code
   ├─ src/xinput1_3.def           exported functions (by name and ordinal)
   ├─ tools/probe.cpp, hooktest.cpp
   ├─ xinput_proxy.ini            default settings
   └─ build.bat
```

---

## FAQ

**Why not use Xidi, Durazno or x360ce?**
They're established tools and work for many people. [Xidi](https://github.com/samuelgr/Xidi)
is a common fix for Conviction, and Durazno is commonly suggested for Future Soldier.
These fixes are an alternative if you want something small enough to read in one
sitting: each solves one problem, with all of its code in one source file. Note
that no other tool's code is used here.

**Does this work with the Ubisoft Connect (non-Steam) version?**
Probably, but it hasn't been tested. The DLLs don't depend on Steam, and the
Future Soldier Steam workaround is harmless when Steam isn't involved.

**Does it work with PlayStation or other controllers?**
Untested. The Conviction fix reports *every* controller as an Xbox 360 pad, which
suits Xbox-style controllers. The Future Soldier fix works with anything Windows
exposes through XInput.

**I updated or verified the game in Steam. Do I need to reinstall?**
No. Verifying restores the game's own files but leaves extra files like these alone.

**What was it tested on?**
Windows 11, the Steam versions of both games, and an Xbox Series X|S controller.

---

## Reporting problems

Please [open an issue](https://github.com/G-Lazer/PCGameControllerFixes/issues) and include:

- the game and where it's installed from (Steam / Ubisoft Connect),
- your controller and how it's connected (USB / Bluetooth / wireless adapter),
- the log file from the game folder (`dinput8_proxy.log` or `xinput_proxy.log`).
  For a detailed log, set `LogInput=1` (Conviction) or `LogStats=1` (Future
  Soldier), play for a minute, then attach it. Logs contain your game's install
  path, which may include your Windows username. Edit that out if you prefer.

---

## License

[MIT](LICENSE). Use, change and share freely.

Not affiliated with or endorsed by Ubisoft, Valve or Microsoft. Game and product
names are trademarks of their owners and are used only to say what these fixes
are for.
