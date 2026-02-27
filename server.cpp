#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>

#include <iostream>
#include <vector>
#include "encryption.h"

#pragma comment(lib, "ws2_32.lib")

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

    CryptoChannel ch;
    if (!ch.init_server(c)) {
      std::cerr << "Handshake failed\n";
      closesocket(c);
      continue;
    }

    while (true) {
      std::vector<uint8_t> pt;
      if (!ch.recv_msg(c, pt)) break;      // client disconnected or auth failed
      if (!ch.send_msg(c, pt)) break;      // echo back
    }

    closesocket(c);
    std::cout << "Client disconnected\n";
  }

  WSACleanup();
  return 0;
}