#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>

#include <atomic>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "encryption.h"
#include "swapping.h"

#pragma comment(lib, "ws2_32.lib")

namespace {

constexpr const char kDiscoveryRequest[] = "SWAPSTER_DISCOVER";
constexpr const char kDiscoveryReady[] = "SWAPSTER_READY";
constexpr const char kDiscoveryBusy[] = "SWAPSTER_BUSY";
constexpr const char kTimeoutReply[] = "TIMEOUT";
constexpr int kDiscoveryPollMs = 250;
constexpr int kAcceptTimeoutMs = 10000;
constexpr int kIdleTimeoutMs = 60 * 1000;

struct DiscoveryPacket {
  std::string payload;
  sockaddr_in sender{};
  std::string local_ip;
};

static bool load_recvmsg_fn(SOCKET sock, LPFN_WSARECVMSG& recvmsg_fn) {
  recvmsg_fn = nullptr;
  GUID guid = WSAID_WSARECVMSG;
  DWORD bytes = 0;
  return WSAIoctl(sock, SIO_GET_EXTENSION_FUNCTION_POINTER,
                  &guid, sizeof(guid),
                  &recvmsg_fn, sizeof(recvmsg_fn),
                  &bytes, nullptr, nullptr) != SOCKET_ERROR;
}

static std::string sockaddr_to_ip(const sockaddr_in& addr) {
  char buffer[32]{};
  const char* text = inet_ntop(AF_INET, &addr.sin_addr, buffer, sizeof(buffer));
  if (!text) {
    return std::string();
  }
  return std::string(text);
}

static bool receive_discovery_packet(SOCKET sock, LPFN_WSARECVMSG recvmsg_fn, DiscoveryPacket& packet) {
  packet.payload.clear();
  packet.local_ip.clear();
  std::memset(&packet.sender, 0, sizeof(packet.sender));

  char data[256]{};

  if (recvmsg_fn) {
    WSABUF buffer{};
    buffer.buf = data;
    buffer.len = sizeof(data);

    char control[WSA_CMSG_SPACE(sizeof(IN_PKTINFO))]{};
    WSAMSG msg{};
    msg.name = reinterpret_cast<SOCKADDR*>(&packet.sender);
    msg.namelen = sizeof(packet.sender);
    msg.lpBuffers = &buffer;
    msg.dwBufferCount = 1;
    msg.Control.buf = control;
    msg.Control.len = sizeof(control);

    DWORD bytes = 0;
    int rc = recvmsg_fn(sock, &msg, &bytes, nullptr, nullptr);
    if (rc == SOCKET_ERROR) {
      return false;
    }

    packet.payload.assign(data, data + bytes);

    for (WSACMSGHDR* cmsg = WSA_CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = WSA_CMSG_NXTHDR(&msg, cmsg)) {
      if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO) {
        auto* pktinfo = reinterpret_cast<IN_PKTINFO*>(WSA_CMSG_DATA(cmsg));
        char ipbuf[32]{};
        const char* text = inet_ntop(AF_INET, &pktinfo->ipi_addr, ipbuf, sizeof(ipbuf));
        if (text) {
          packet.local_ip = text;
        }
        break;
      }
    }

    return true;
  }

  int sender_len = sizeof(packet.sender);
  int received = recvfrom(sock, data, sizeof(data), 0,
                          reinterpret_cast<sockaddr*>(&packet.sender), &sender_len);
  if (received == SOCKET_ERROR) {
    return false;
  }

  packet.payload.assign(data, data + received);
  return true;
}

static bool send_udp_reply(SOCKET udp_sock, const sockaddr_in& client, const char* text) {
  int sent = sendto(udp_sock, text, static_cast<int>(std::strlen(text)), 0,
                    reinterpret_cast<const sockaddr*>(&client), sizeof(client));
  return sent != SOCKET_ERROR;
}

static SOCKET create_tcp_listener(const std::string& local_ip, int port) {
  SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock == INVALID_SOCKET) {
    return INVALID_SOCKET;
  }

  BOOL reuse = TRUE;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<u_short>(port));
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    closesocket(sock);
    return INVALID_SOCKET;
  }

  if (listen(sock, 1) == SOCKET_ERROR) {
    closesocket(sock);
    return INVALID_SOCKET;
  }

  return sock;
}

static SOCKET accept_with_timeout(SOCKET listener, int timeout_ms) {
  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(listener, &readfds);

  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;

  int ready = select(0, &readfds, nullptr, nullptr, &tv);
  if (ready <= 0) {
    if (ready == 0) {
      WSASetLastError(WSAETIMEDOUT);
    }
    return INVALID_SOCKET;
  }

  sockaddr_in client{};
  int client_len = sizeof(client);
  return accept(listener, reinterpret_cast<sockaddr*>(&client), &client_len);
}

static bool send_text_reply(CryptoChannel& channel, SOCKET sock, const char* text) {
  std::vector<uint8_t> payload(text, text + std::strlen(text));
  return channel.send_msg(sock, payload);
}

static void session_thread(SOCKET listener,
                           std::atomic<bool>& shutdown_requested,
                           std::atomic<bool>& session_active,
                           std::atomic<bool>& awaiting_accept,
                           std::atomic<bool>& session_established) {
  SOCKET client = accept_with_timeout(listener, kAcceptTimeoutMs);
  if (client == INVALID_SOCKET) {
    awaiting_accept.store(false);
    session_established.store(false);
    closesocket(listener);
    session_active.store(false);
    return;
  }

  awaiting_accept.store(false);
  session_established.store(true);

  DWORD idle_timeout = kIdleTimeoutMs;
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&idle_timeout), sizeof(idle_timeout));

  CryptoChannel channel;
  if (!channel.init_server(client)) {
    session_established.store(false);
    closesocket(client);
    closesocket(listener);
    session_active.store(false);
    return;
  }

  while (!shutdown_requested.load()) {
    std::vector<uint8_t> plaintext;
    if (!channel.recv_msg(client, plaintext)) {
      int error = WSAGetLastError();
      if (error == WSAETIMEDOUT) {
        send_text_reply(channel, client, kTimeoutReply);
      }
      break;
    }

    std::string message(plaintext.begin(), plaintext.end());

    if (message == "SWAP") {
      swapster::SwapAllWindows();
      send_text_reply(channel, client, "Windows swapped successfully");
    } else if (message == "TERM") {
      send_text_reply(channel, client, "Server shutting down");
      shutdown_requested.store(true);
      break;
    } else {
      send_text_reply(channel, client, "Unknown command");
    }
  }

  shutdown(client, SD_BOTH);
  session_established.store(false);
  closesocket(client);
  closesocket(listener);
  session_active.store(false);
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: swapster.exe <port>\n";
    return 1;
  }

  int port = std::atoi(argv[1]);

  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    std::cerr << "WSAStartup failed\n";
    return 1;
  }

  HANDLE mutex = CreateMutexA(NULL, TRUE, "Global\\SwapsterServer");
  if (!mutex) {
    std::cerr << "Failed to create server mutex\n";
    WSACleanup();
    return 1;
  }

  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(mutex);
    WSACleanup();
    return 0;
  }

  SOCKET udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (udp_sock == INVALID_SOCKET) {
    std::cerr << "Failed to create UDP socket\n";
    CloseHandle(mutex);
    WSACleanup();
    return 1;
  }

  BOOL reuse = TRUE;
  setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

  DWORD timeout = kDiscoveryPollMs;
  setsockopt(udp_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

  DWORD pktinfo = 1;
  setsockopt(udp_sock, IPPROTO_IP, IP_PKTINFO, reinterpret_cast<const char*>(&pktinfo), sizeof(pktinfo));

  sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_port = htons(static_cast<u_short>(port));
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(udp_sock, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) == SOCKET_ERROR) {
    std::cerr << "Failed to bind UDP discovery socket\n";
    closesocket(udp_sock);
    CloseHandle(mutex);
    WSACleanup();
    return 1;
  }

  LPFN_WSARECVMSG recvmsg_fn = nullptr;
  load_recvmsg_fn(udp_sock, recvmsg_fn);

  std::atomic<bool> shutdown_requested{false};
  std::atomic<bool> session_active{false};
  std::atomic<bool> awaiting_accept{false};
  std::atomic<bool> session_established{false};
  std::atomic<u_long> pending_client_addr{0};

  while (!shutdown_requested.load()) {
    DiscoveryPacket packet;
    if (!receive_discovery_packet(udp_sock, recvmsg_fn, packet)) {
      int error = WSAGetLastError();
      if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) {
        continue;
      }
      continue;
    }

    if (packet.payload != kDiscoveryRequest) {
      continue;
    }

    if (session_active.load()) {
      if (awaiting_accept.load()) {
        send_udp_reply(udp_sock, packet.sender, kDiscoveryReady);
      } else {
        if (session_established.load()) {
          send_udp_reply(udp_sock, packet.sender, kDiscoveryBusy);
        } else {
          send_udp_reply(udp_sock, packet.sender, kDiscoveryReady);
        }
      }
      continue;
    }

    SOCKET listener = create_tcp_listener(packet.local_ip, port);
    if (listener == INVALID_SOCKET) {
      send_udp_reply(udp_sock, packet.sender, kDiscoveryBusy);
      continue;
    }

    pending_client_addr.store(packet.sender.sin_addr.s_addr);
    session_active.store(true);
    awaiting_accept.store(true);
    std::thread(session_thread, listener,
                std::ref(shutdown_requested),
                std::ref(session_active),
                std::ref(awaiting_accept),
                std::ref(session_established)).detach();
    send_udp_reply(udp_sock, packet.sender, kDiscoveryReady);
  }

  closesocket(udp_sock);
  CloseHandle(mutex);
  WSACleanup();
  return 0;
}
