# Building Clean HUD

Clean HUD is a single-file ScriptHookRDR2 plugin (`CleanHUD.cpp`).

## Requirements
- Windows, Visual Studio 2019/2022 (Desktop development with C++), Windows 10/11 SDK
- ScriptHookRDR2 SDK (`main.h`, `natives.h`, `ScriptHookRDR2.lib`) from http://www.dev-c.com/rdr2/scripthookrdr2/

## Steps
1. Create a new **Dynamic-Link Library (DLL)** project (empty), platform **x64**.
2. Add `CleanHUD.cpp` to the project.
3. Project Properties (configuration **Release**, platform **x64**):
   - C/C++ > Language > C++ Language Standard: **ISO C++17**
   - C/C++ > Code Generation > Runtime Library: **Multi-threaded (/MT)** (no VC++ redistributable needed)
   - C/C++ > General > Additional Include Directories: the folder with the ScriptHookRDR2 SDK headers
   - Linker > Input > Additional Dependencies: `ScriptHookRDR2.lib`
     (`xinput.lib` is pulled in automatically by a `#pragma comment(lib, ...)` in the source)
   - Advanced > Target File Extension: **.asi**
4. Build. Output: `CleanHUD.asi`.

## Run
- Install ScriptHookRDR2 and an ASI loader as usual.
- Put `CleanHUD.asi` and `CleanHUD.ini` in the Red Dead Redemption 2 game folder.

## What the plugin touches
- Reads keyboard keys (ScriptHookRDR2 keyboard callback) and XInput controller state.
- Calls in-game HUD natives (hide HUD/radar, HUD contexts, draw a dot and a toggle message).
- Reads `CleanHUD.ini`, writes `CleanHUD.log` next to the `.asi`.
- No network access, no other files, no process injection, no dynamic library loading.
