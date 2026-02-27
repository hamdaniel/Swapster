#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>

#include <iostream>
#include <string>
#include <vector>
#include "encryption.h"

#pragma comment(lib, "ws2_32.lib")

static SOCKET connect_to(const char* ip, int port) {
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return INVALID_SOCKET;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((u_short)port);
  addr.sin_addr.s_addr = inet_addr(ip); // MinGW.org compatible
  if (addr.sin_addr.s_addr == INADDR_NONE) return INVALID_SOCKET;

  if (connect(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) return INVALID_SOCKET;
  return s;
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Usage: client.exe <server_ipv4> <port>\n";
    return 1;
  }

  const char* ip = argv[1];
  int port = std::atoi(argv[2]);

  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
    std::cerr << "WSAStartup failed\n";
    return 1;
  }

  SOCKET s = connect_to(ip, port);
  if (s == INVALID_SOCKET) {
    std::cerr << "Connect failed\n";
    WSACleanup();
    return 1;
  }

  CryptoChannel ch;
  if (!ch.init_client(s)) {
    std::cerr << "Handshake failed\n";
    closesocket(s);
    WSACleanup();
    return 1;
  }

  std::cout << "Connected. Type a line and press Enter. Ctrl+Z then Enter to quit.\n";

  std::string line;
  while (std::getline(std::cin, line)) {
    std::vector<uint8_t> pt(line.begin(), line.end());

    if (!ch.send_msg(s, pt)) {
      std::cerr << "Send failed\n";
      break;
    }

    std::vector<uint8_t> echo_pt;
    if (!ch.recv_msg(s, echo_pt)) {
      std::cerr << "Receive failed\n";
      break;
    }

    std::cout << "echo: " << std::string(echo_pt.begin(), echo_pt.end()) << "\n";
  }

  closesocket(s);
  WSACleanup();
  return 0;
}