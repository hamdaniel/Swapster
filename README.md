# Swapster

Swapster is a Windows server/client pair for remote monitor-window swapping over an encrypted channel.

## Project Layout

- [src](src): C++ source files
- [include](include): C++ headers
- [scripts/installer.bat](scripts/installer.bat): canonical installer script source
- [swapster_version.rc](swapster_version.rc): Windows version resource for server binary
- [Makefile](Makefile): MinGW build
- [SwapsterInstaller](SwapsterInstaller): generated distribution output

## Build

Requirements:
- Windows
- MinGW-w64 (`x86_64-w64-mingw32-g++`)
- `windres` at `C:\msys64\mingw64\bin\windres.exe`
- `ncat` available in PATH (used by LAN probing)

Build (from repo root):

```powershell
mingw32-make clean
mingw32-make
```

Output:
- `SwapsterInstaller\swapster.exe`
- `SwapsterInstaller\controller.exe`
- `SwapsterInstaller\installer.bat`

## Install Server on Target Machine

1. Copy the full [SwapsterInstaller](SwapsterInstaller) folder to the target machine.
2. Run `SwapsterInstaller\installer.bat` as Administrator (it self-elevates if needed).

Installer actions:
- Copies server to `%ProgramData%\Swapster\swapster.exe`
- Creates scheduled task `Swapster` (run on logon)
- Creates scheduled task `Swapster_Unlock` (run on unlock event 4801)
- Adds Windows Firewall allow rule for the installed server exe
- Starts server immediately on port `2003`

## Controller Usage

From [SwapsterInstaller](SwapsterInstaller):

Auto-discovery:

```cmd
controller.exe
```

Direct connect:

```cmd
controller.exe <server_ip> 2003
```

Commands after connection:
- `SWAP` -> swaps windows on target machine
- `TERM` -> triggers server cleanup/uninstall flow
- `EXIT` -> disconnects controller

## TERM / Cleanup Behavior

When `TERM` is sent:
- Server launches cleanup mode
- Deletes scheduled tasks (`Swapster`, `Swapster_Unlock`)
- Stops other running `swapster.exe` instances
- Deletes the currently running server executable path
- Removes `%ProgramData%\Swapster`

If server was started via installer, this removes the ProgramData-installed copy, not the distribution copy in [SwapsterInstaller](SwapsterInstaller).

## Notes

- LAN discovery currently probes hosts using ARP/ping + `ncat` magic probe.
- The installer expects `swapster.exe` to be in the same folder as `installer.bat` when run.
