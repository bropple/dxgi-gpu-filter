# dxgi-gpu-filter

A tiny DXGI proxy DLL that controls **which GPUs a game is allowed to see**.

Some games (notably several Unreal Engine titles) pick their own graphics adapter
and ignore Windows' per-app GPU setting, the NVIDIA/AMD control panels, and
launch options. On a PC with two GPUs, they can end up rendering on the wrong one.
This DLL sits next to the game's exe as `dxgi.dll` and removes the unwanted GPUs
from the list the game gets, so the game can only choose the one you want.

> Status: works on the author's machine (RTX 4080 + RX 9060 XT, S.T.A.L.K.E.R. 2).
> Everything else is untested. Please open an issue with your `dxgi_filter.log`.

## Use only in single-player games
A `dxgi.dll` proxy is the same technique ReShade uses. Anti-cheat systems in online
games may flag it and you could be banned. **Use it at your own risk, offline games only.**

## Install
1. Download **`dxgi.dll`**, `install.ps1` and `SHA256SUMS.txt` from the
   [Releases](../../releases) page. The DLL is built by GitHub Actions from the
   source in this repo; no binaries are committed.
2. (Optional) verify it:
   - `Get-FileHash dxgi.dll -Algorithm SHA256` should match `SHA256SUMS.txt`
   - `gh attestation verify dxgi.dll --repo bropple/dxgi-gpu-filter` checks the build provenance
3. Run the installer and pick your GPU:
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\install.ps1 -Target "<folder containing the game's exe>"
   ```
   For Unreal games that's the folder with `*-Win64-Shipping.exe`, e.g.
   `...\Game\Binaries\Win64\`.

Or by hand: copy `dxgi.dll` there and create `dxgi_filter.ini` from
`dxgi_filter.ini.example`.

## Config (`dxgi_filter.ini`)
```ini
[Filter]
KeepNameContains=RTX 4080      ; keep mode: only matching adapters stay visible
;KeepVendorIds=10DE            ; and/or by vendor ID (hex)
;HideVendorIds=1002,1414       ; hide mode (only if no Keep* is set)
HideDuplicateWithoutOutputs=1  ; hide a second copy of the same GPU that has no outputs
Log=1                          ; write dxgi_filter.log
```
Vendor IDs: `10DE` NVIDIA, `1002` AMD, `8086` Intel, `1414` Microsoft Basic Render Driver.
If the rules would hide **every** GPU, nothing is hidden.

## Troubleshooting
- Look at `dxgi_filter.log` next to the DLL. It lists every adapter the game asked
  about, with vendor/device IDs, VRAM, whether it has display outputs, and whether it was kept.
- No log file at all: the game isn't loading the DLL (wrong folder, or the game uses a different exe).
- If a game saved a GPU choice by index in its settings, the index may no longer match the shorter
  list. Reset the game's adapter setting to "automatic".
- To undo everything: run `install.ps1 -Uninstall -Target ...` or delete `dxgi.dll`,
  `dxgi_filter.ini` and `dxgi_filter.log`.

## How it works
- The game loads this `dxgi.dll` from its own folder; the DLL loads the real one from System32 and
  forwards every export to it.
- `CreateDXGIFactory`, `CreateDXGIFactory1` and `CreateDXGIFactory2` are wrapped; after the real call
  the factory's COM vtable is patched so `EnumAdapters`, `EnumAdapters1`, `EnumAdapterByLuid` and
  `EnumAdapterByGpuPreference` skip the filtered adapters.

## Limits
- 64-bit only.
- Doesn't affect code that never goes through DXGI enumeration (e.g. `D3D12CreateDevice(nullptr, ...)`)
  or vendor libraries that query the driver directly.
- Can't run on top of another `dxgi.dll` mod (ReShade etc.) without extra work.

## Build locally
```bash
# Linux, MSYS2 MinGW64, or anything with mingw-w64
x86_64-w64-mingw32-g++ -O2 -std=c++17 -shared -static -o dxgi.dll src/dxgi_filter.cpp src/dxgi.def
```
Releases are built by `.github/workflows/build.yml` on every `v*` tag.

## License
MIT. See `LICENSE`.
