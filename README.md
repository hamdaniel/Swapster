# Swapster

Swapster provides encrypted remote monitor-window swapping between Windows x64 machines on the same LAN.

## Project Layout

- [src](src): C++ source files
- [include](include): C++ headers
- [scripts/installer.bat](scripts/installer.bat): canonical installer script source
- [swapster_version.rc](swapster_version.rc): Windows version resource for server binary
- [Makefile](Makefile): MinGW build
- `SwapsterInstaller`: generated distribution output

## Build

Requirements:
- Windows
- MinGW-w64 (`x86_64-w64-mingw32-g++`)
- `windres` at `C:\msys64\mingw64\bin\windres.exe`

Build (from repo root):

```powershell
mingw32-make clean
mingw32-make
```

For debug builds with logging enabled:

```powershell
mingw32-make clean
mingw32-make L=1
```

When built with `L=1`, the server logs events to `C:\ProgramData\Swapster\swapster_log.txt` including:
- Server startup with port number
- Controller connections/disconnections with IP addresses
- Window swap operations
- Critical errors

Output:
- `SwapsterInstaller\swapster.exe`
- `SwapsterInstaller\controller.exe`
- `SwapsterInstaller\installer.bat`

## Install Server on Target Machine

1. Move the `SwapsterInstaller` folder to the target machine.
2. Run `SwapsterInstaller\installer.bat` as Administrator (it self-elevates if needed).

Installer actions:
- Copies server to `%ProgramData%\Swapster\swapster.exe`
- Creates scheduled task `Swapster` (run on logon)
- Creates scheduled task `Swapster_Unlock` (run on unlock event 4801)
- Adds Windows Firewall allow rules:
  - **TCP port 2003**: For encrypted client communication
  - **UDP port 2003**: For broadcast discovery
- Starts server immediately on port `2003`

## Controller Usage

From `SwapsterInstaller`:

Auto-discovery:

```cmd
controller.exe
```

The controller searches for Swapster servers on the local network by:
1. Broadcasting `SWAPSTER_DISCOVER` on UDP port 2003 to all network adapters
2. Listening for `SWAPSTER_HERE` response from the server
3. Automatically connecting to the first server that responds

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
- Removes Windows Firewall rules (TCP and UDP)
- Stops other running `swapster.exe` instances
- Deletes the currently running server executable path
- Removes `%ProgramData%\Swapster`

## Troubleshooting

### Server Not Found During Discovery

If auto-discovery fails to find the server:
- **Both machines must be on the same subnet** - UDP broadcast only works within a local network segment
- **Multiple network adapters**: The controller tries all adapters, prioritizing those with gateways (real adapters over virtual ones like VMware/VirtualBox)
- Check that Windows Firewall allows **UDP port 2003** on the server (the installer creates this rule automatically)
- Verify the firewall rules exist on server:
  ```cmd
  netsh advfirewall firewall show rule name="Swapster Discovery"
  netsh advfirewall firewall show rule name="Swapster Server"
  ```
- Try direct connection if on different subnets: `controller.exe <server_ip> 2003`
- Verify server is running: check Task Manager for `swapster.exe`
- If built with `L=1`, check logs at `C:\ProgramData\Swapster\swapster_log.txt` on the server

## Notes

- **Multi-adapter Support**: Controller automatically tries all network adapters, prioritizing real adapters (with gateways) over virtual ones
- **Encryption**: All commands use AES-256-CTR encryption with HMAC-SHA256 authentication after initial handshake
- The installer expects `swapster.exe` to be in the same folder as `installer.bat` when run
- The installer can be run from a USB drive
- If multiple Swapster servers are on the same LAN, auto-discovery connects to the first one that responds
