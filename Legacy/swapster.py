import subprocess
import sys
from pathlib import Path

"""
Swapster launcher

Usage:
  python swapster.py --server   # build (if needed) and start server in background
  python swapster.py            # build (if needed) and trigger swap

Notes:
- This script recompiles when the .cpp source is newer than the .exe.
- Tested with MinGW g++ (older toolchains) using -std=c++11 and Win32 APIs.
"""

EXE_SERVER = Path("swapster_server.exe")
EXE_CLIENT = Path("swapster_client.exe")

SRC_SERVER = Path("swapster_server.cpp")
SRC_CLIENT = Path("swapster_client.cpp")


def needs_rebuild(src: Path, exe: Path) -> bool:
    if not exe.exists():
        return True
    try:
        return src.stat().st_mtime > exe.stat().st_mtime
    except OSError:
        return True


def compile_cpp(src: Path, exe: Path, extra_libs=None):
    extra_libs = extra_libs or []
    cmd = ["g++", str(src), "-o", str(exe), "-mconsole", "-std=c++11"] + extra_libs
    subprocess.check_call(cmd)


def main() -> int:
    run_server = "--server" in sys.argv

    if needs_rebuild(SRC_SERVER, EXE_SERVER):
        print("Compiling server...")
        compile_cpp(SRC_SERVER, EXE_SERVER, extra_libs=["-lgdi32"])

    if needs_rebuild(SRC_CLIENT, EXE_CLIENT):
        print("Compiling client...")
        compile_cpp(SRC_CLIENT, EXE_CLIENT)

    if run_server:
        print("Starting server in background...")
        subprocess.Popen([str(EXE_SERVER)])
        print("Server started.")
    else:
        subprocess.call([str(EXE_CLIENT)])

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
