#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>

#include <iostream>
#include <vector>
#include <shellapi.h>
#include "encryption.h"
#include "swapping.h"

#pragma comment(lib, "ws2_32.lib")

#include <windows.h>
#include <shellapi.h>
#include <string>

#ifdef LOG
#define LOGF(fmt, ...) do { \
  FILE* f = fopen("C:\\ProgramData\\Swapster\\swapster_log.txt", "a"); \
  if (!f) { \
    char b[128]; \
    wsprintfA(b, "LOGF fopen failed gle=%lu\n", GetLastError()); \
    OutputDebugStringA(b); \
  } else { \
    fprintf(f, fmt "\n", ##__VA_ARGS__); fclose(f); \
  } \
} while(0)
#else
#define LOGF(fmt, ...) do { } while(0)
#endif

static bool handle_magic_probe(SOCKET c) {
    static const char kMagic[] = "swapsterswapster"; // 16 bytes
    static const char kReply[] = "SwapsterServerOK"; // reply to send
    const DWORD deadline_ms = GetTickCount() + 250;  // small pre-handshake window

    while (GetTickCount() < deadline_ms) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(c, &rfds);

        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 50 * 1000;

        int sel = select(0, &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        u_long avail = 0;
        if (ioctlsocket(c, FIONREAD, &avail) != 0) return false;
        if (avail < 16) continue;

        char buf[16];
        int r = recv(c, buf, 16, MSG_PEEK);
        if (r != 16) return false;

        if (memcmp(buf, kMagic, 16) == 0) {
            // consume the magic string
            recv(c, buf, 16, 0);

            // send fixed plaintext reply
            send(c, kReply, sizeof(kReply) - 1, 0);
            
            // Give client time to receive before closing
            Sleep(100);

            return true; // caller will close
        }

        return false;
    }

    return false;
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

  // Remove firewall rules
  system("netsh advfirewall firewall delete rule name=\"Swapster Server\" >nul 2>&1");
  system("netsh advfirewall firewall delete rule name=\"Swapster Discovery\" >nul 2>&1");

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
  if (listen(ls, 0) == SOCKET_ERROR) return INVALID_SOCKET;
  return ls;
}

// UDP discovery listener thread
static DWORD WINAPI udp_discovery_thread(LPVOID param) {
  int port = *(int*)param;
  
  SOCKET udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (udp_sock == INVALID_SOCKET) {
    LOGF("UDP discovery socket creation failed");
    return 1;
  }
  
  // Allow broadcast
  BOOL broadcast = TRUE;
  setsockopt(udp_sock, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcast, sizeof(broadcast));
  
  sockaddr_in udp_addr{};
  udp_addr.sin_family = AF_INET;
  udp_addr.sin_port = htons((u_short)port);
  udp_addr.sin_addr.s_addr = INADDR_ANY;
  
  if (bind(udp_sock, (sockaddr*)&udp_addr, sizeof(udp_addr)) == SOCKET_ERROR) {
    LOGF("UDP bind failed err=%d", WSAGetLastError());
    closesocket(udp_sock);
    return 1;
  }
  
  LOGF("UDP discovery listening on port %d", port);
  
  char buf[64];
  sockaddr_in client_addr;
  int client_addr_len = sizeof(client_addr);
  
  while (true) {
    int received = recvfrom(udp_sock, buf, sizeof(buf) - 1, 0, 
                            (sockaddr*)&client_addr, &client_addr_len);
    
    if (received > 0) {
      buf[received] = '\0';
      
      if (strcmp(buf, "SWAPSTER_DISCOVER") == 0) {
        LOGF("Discovery request from %s", inet_ntoa(client_addr.sin_addr));
        
        const char* response = "SWAPSTER_HERE";
        sendto(udp_sock, response, (int)strlen(response), 0, 
               (sockaddr*)&client_addr, client_addr_len);
        
        LOGF("Sent discovery response");
      }
    }
  }
  
  closesocket(udp_sock);
  return 0;
}

int main(int argc, char** argv) {

  LOGF("Server start argc=%d", argc);

  // singleton guard: only one instance per session/host
  HANDLE hMutex = CreateMutexA(NULL, TRUE, "Global\\SwapsterServer");
  if (!hMutex) {
    LOGF("CreateMutex failed gle=%lu", GetLastError());
  } else if (GetLastError() == ERROR_ALREADY_EXISTS) {
    LOGF("Another instance already running, exiting");
    return 0;
  }

  // ---- Cleanup mode ----
  if (argc == 2 && std::string(argv[1]) == "--cleanup") {
    LOGF("Cleanup mode");
    return run_cleanup();
  }

  // ---- Validate args ----
  if (argc != 2) {
    LOGF("Invalid argc=%d", argc);
    return 1;
  }

  LOGF("Port arg=%s", argv[1]);
  int port = std::atoi(argv[1]);

  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
    LOGF("WSAStartup failed err=%d", WSAGetLastError());
    return 1;
  }
  LOGF("WSAStartup OK");

  SOCKET ls = make_listen_socket(port);
  if (ls == INVALID_SOCKET) {
    LOGF("Bind/listen failed err=%d", WSAGetLastError());
    WSACleanup();
    return 1;
  }

  LOGF("Listening on %d", port);
  
  // Start UDP discovery listener thread
  static int discovery_port = port;
  HANDLE hDiscoveryThread = CreateThread(NULL, 0, udp_discovery_thread, &discovery_port, 0, NULL);
  if (hDiscoveryThread) {
    LOGF("UDP discovery thread started");
    CloseHandle(hDiscoveryThread); // detach thread
  } else {
    LOGF("Failed to start UDP discovery thread");
  }

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
      if (!ch.recv_msg(c, pt)) break;

      std::string msg(reinterpret_cast<const char*>(pt.data()), pt.size());

      if (msg == "SWAP") {
        LOGF("SWAP command received");
        swapster::SwapAllWindows();
        LOGF("SWAP command executed");
        const char* reply = "Swapping windows...";
        std::vector<uint8_t> out(reply, reply + strlen(reply));
        if (!ch.send_msg(c, out)) break;
      }
      else if (msg == "TERM") {
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

        return 0;
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