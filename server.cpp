#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>

#include <iostream>
#include <vector>
#include <shellapi.h>
#include "encryption.h"
#include "swapster.h"

#pragma comment(lib, "ws2_32.lib")

#include <windows.h>
#include <shellapi.h>
#include <string>

static bool IsWorkstationLocked_ByDesktopName() {
    // Open the currently active desktop (the one receiving user input)
    HDESK hDesk = OpenInputDesktop(0, FALSE, GENERIC_READ);
    if (!hDesk) {
        // If we can't open the input desktop, treat as locked/restricted
        return true;
    }

    char name[256] = {};
    DWORD needed = 0;
    bool locked = true;

    if (GetUserObjectInformationA(hDesk, UOI_NAME, name, sizeof(name), &needed)) {
        // When unlocked, this is typically "Default"
        // When locked, it's typically "Winlogon"
        locked = (_stricmp(name, "Default") != 0);
    }

    CloseDesktop(hDesk);
    return locked;
}

static bool handle_magic_probe(SOCKET c) {
    static const char kMagic[] = "swapsterswapster"; // 16 bytes
    const DWORD deadline_ms = GetTickCount() + 250;  // small pre-handshake window

    while (GetTickCount() < deadline_ms) {
        // Wait up to 50ms for data to become readable
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(c, &rfds);

        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 50 * 1000;

        int sel = select(0, &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue; // timeout or error -> keep trying until deadline

        // Data is readable; only proceed if we have at least 16 bytes buffered
        u_long avail = 0;
        if (ioctlsocket(c, FIONREAD, &avail) != 0) return false;
        if (avail < 16) continue;

        char buf[16];
        int r = recv(c, buf, 16, MSG_PEEK);
        if (r != 16) return false;

        if (memcmp(buf, kMagic, 16) == 0) {
            // consume + echo + tell caller to close
            recv(c, buf, 16, 0);
            send(c, buf, 16, 0);
            return true;
        }

        return false; // first 16 bytes are not magic -> proceed with normal handshake
    }

    return false; // no early client data -> proceed with normal handshake (server sends nonce first)
}

static bool is_admin() {
  BOOL isAdmin = FALSE;
  PSID adminGroup = NULL;
  SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;

  if (AllocateAndInitializeSid(&NtAuthority, 2,
      SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
      0,0,0,0,0,0, &adminGroup)) {
    CheckTokenMembership(NULL, adminGroup, &isAdmin);
    FreeSid(adminGroup);
  }
  return isAdmin != FALSE;
}

static int run_cleanup() {
  // Elevate if needed
  if (!is_admin()) {
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    ShellExecuteA(NULL, "runas", exe, "--cleanup", NULL, SW_HIDE);
    return 0;
  }

  const char* TASK1 = "\\Swapster";
  const char* TASK2 = "\\Swapster_Unlock";

  // Delete tasks (do this before killing processes)
  system("schtasks /delete /tn \"\\Swapster\" /f >nul 2>&1");
  system("schtasks /delete /tn \"\\Swapster_Unlock\" /f >nul 2>&1");

  // Kill other instances (NOT this one)
  DWORD selfPid = GetCurrentProcessId();
  {
    std::string kill = "taskkill /IM swapster.exe /F /FI \"PID ne ";
    kill += std::to_string(selfPid);
    kill += "\" >nul 2>&1";
    system(kill.c_str());
  }

  // Path to this exe
  char exePath[MAX_PATH];
  GetModuleFileNameA(NULL, exePath, MAX_PATH);

  // After we exit: delete exe, then remove folder
  std::string cmd =
    "cmd /C ping 127.0.0.1 -n 3 >nul & "
    "del /F /Q \"";
  cmd += exePath;
  cmd += "\" >nul 2>&1 & "
         "rmdir /S /Q \"C:\\ProgramData\\Swapster\" >nul 2>&1";

  STARTUPINFOA si{};
  PROCESS_INFORMATION pi{};
  si.cb = sizeof(si);

  CreateProcessA(
    NULL, (LPSTR)cmd.c_str(),
    NULL, NULL, FALSE,
    CREATE_NO_WINDOW | DETACHED_PROCESS,
    NULL, NULL, &si, &pi
  );

  if (pi.hProcess) CloseHandle(pi.hProcess);
  if (pi.hThread)  CloseHandle(pi.hThread);

  return 0;
}

static SOCKET make_listen_socket(int port) {
  SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (ls == INVALID_SOCKET) return INVALID_SOCKET;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((u_short)port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(ls, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) return INVALID_SOCKET;
  if (listen(ls, 16) == SOCKET_ERROR) return INVALID_SOCKET;
  return ls;
}

int main(int argc, char** argv) {

  // ---- Cleanup mode ----
  if (argc == 2 && std::string(argv[1]) == "--cleanup") {
    return run_cleanup();
  }

  if (argc != 2) {
    std::cerr << "Usage: server.exe <port>\n";
    return 1;
  }
  int port = std::atoi(argv[1]);

  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
    std::cerr << "WSAStartup failed\n";
    return 1;
  }

  SOCKET ls = make_listen_socket(port);
  if (ls == INVALID_SOCKET) {
    std::cerr << "Failed to bind/listen\n";
    WSACleanup();
    return 1;
  }

  std::cout << "Listening on 0.0.0.0:" << port << "\n";

  while (true) {
    SOCKET c = accept(ls, NULL, NULL);
    if (c == INVALID_SOCKET) continue;

    std::cout << "Client connected\n";

    if (handle_magic_probe(c)) {
        closesocket(c);
        std::cout << "Magic probe handled\n";
        continue;
    }

    CryptoChannel ch;
    if (!ch.init_server(c)) {
      std::cerr << "Handshake failed\n";
      closesocket(c);
      continue;
    }

    while (true) {
      std::vector<uint8_t> pt;
      if (!ch.recv_msg(c, pt)) break; // client disconnected or auth failed

      // Interpret plaintext as a string (safe even if it contains '\0' because we use size)
      std::string msg(reinterpret_cast<const char*>(pt.data()), pt.size());
      if(IsWorkstationLocked_ByDesktopName())
      {
        const char* reply = "Target Computer is locked.";
        std::vector<uint8_t> out(reply, reply + strlen(reply));
        if (!ch.send_msg(c, out)) break;
        continue;
      }
      if (msg == "SWAP") { // trigger the window swap
        swapster::SwapAllWindows();
        const char* reply = "Swapping windows...";
        std::vector<uint8_t> out(reply, reply + strlen(reply));
        if (!ch.send_msg(c, out)) break;
      }
      else if (msg == "TERM")
      {
        char path[MAX_PATH];
        GetModuleFileNameA(NULL, path, MAX_PATH);

        std::string cmd = "\"";
        cmd += path;
        cmd += "\" --cleanup";

        STARTUPINFOA si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);

        CreateProcessA(
            NULL,
            (LPSTR)cmd.c_str(),
            NULL, NULL,
            FALSE,
            CREATE_NO_WINDOW | DETACHED_PROCESS,
            NULL, NULL,
            &si, &pi
        );

        if (pi.hProcess) CloseHandle(pi.hProcess);
        if (pi.hThread)  CloseHandle(pi.hThread);

        return 0; // exit service process
      }
      else {
        const char* reply = "Unknown command";
        std::vector<uint8_t> out(reply, reply + strlen(reply));
        if (!ch.send_msg(c, out)) break;
      }
    }

    closesocket(c);
    std::cout << "Client disconnected\n";
  }

  WSACleanup();
  return 0;
}