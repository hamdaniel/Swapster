#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "encryption.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace {

constexpr const char kDiscoveryRequest[] = "SWAPSTER_DISCOVER";
constexpr const char kDiscoveryReady[] = "SWAPSTER_READY";
constexpr const char kDiscoveryBusy[] = "SWAPSTER_BUSY";
constexpr const char kTimeoutReply[] = "TIMEOUT";
constexpr int kDiscoveryTimeoutMs = 2000;
constexpr int kCommandTimeoutMs = 1000;

struct BroadcastTarget {
  std::string local_ip;
  std::string broadcast_ip;
};

enum class DiscoveryStatus {
  Ready,
  Busy,
  NotFound,
  Invalid
};

struct DiscoveryResult {
  DiscoveryStatus status = DiscoveryStatus::NotFound;
  std::string server_ip;
  std::string message;
};

static void enable_ansi_colors() {
  HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
  if (hOut == INVALID_HANDLE_VALUE) {
    return;
  }

  DWORD mode = 0;
  if (!GetConsoleMode(hOut, &mode)) {
    return;
  }

  mode |= 0x0004;
  SetConsoleMode(hOut, mode);
}

static bool parse_ipv4(const std::string& text, ULONG& out_host_order) {
  unsigned long raw = inet_addr(text.c_str());
  if (raw == INADDR_NONE && text != "255.255.255.255") {
    return false;
  }

  out_host_order = ntohl(raw);
  return true;
}

static std::vector<BroadcastTarget> build_broadcast_targets() {
  std::vector<BroadcastTarget> targets;

  ULONG size = 0;
  if (GetAdaptersInfo(nullptr, &size) != ERROR_BUFFER_OVERFLOW) {
    targets.push_back({"", "255.255.255.255"});
    return targets;
  }

  std::vector<uint8_t> buffer(size);
  auto* adapters = reinterpret_cast<PIP_ADAPTER_INFO>(buffer.data());
  if (GetAdaptersInfo(adapters, &size) != NO_ERROR) {
    targets.push_back({"", "255.255.255.255"});
    return targets;
  }

  for (PIP_ADAPTER_INFO adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
    for (PIP_ADDR_STRING ip_addr = &adapter->IpAddressList; ip_addr != nullptr; ip_addr = ip_addr->Next) {
      std::string local_ip(ip_addr->IpAddress.String);
      std::string mask(ip_addr->IpMask.String);

      if (local_ip.empty() || local_ip == "0.0.0.0" || local_ip.rfind("127.", 0) == 0) {
        continue;
      }

      ULONG local_host = 0;
      ULONG mask_host = 0;
      if (!parse_ipv4(local_ip, local_host) || !parse_ipv4(mask, mask_host) || mask_host == 0) {
        continue;
      }

      ULONG broadcast_host = (local_host & mask_host) | (~mask_host);
      in_addr broadcast_addr{};
      broadcast_addr.s_addr = htonl(broadcast_host);

      char broadcast_text[32]{};
      const char* rendered = inet_ntop(AF_INET, &broadcast_addr, broadcast_text, sizeof(broadcast_text));
      if (!rendered) {
        continue;
      }

      targets.push_back({local_ip, broadcast_text});
    }
  }

  if (targets.empty()) {
    targets.push_back({"", "255.255.255.255"});
  }

  return targets;
}

static std::string ip_to_string(const sockaddr_in& addr) {
  char buffer[32]{};
  const char* text = inet_ntop(AF_INET, &addr.sin_addr, buffer, sizeof(buffer));
  if (!text) {
    return std::string();
  }
  return std::string(text);
}

static SOCKET create_discovery_socket(const std::string& local_ip, bool broadcast) {
  SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock == INVALID_SOCKET) {
    return INVALID_SOCKET;
  }

  BOOL reuse = TRUE;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

  if (broadcast) {
    BOOL enabled = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
  }

  DWORD timeout = 250;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

  sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_port = 0;
  if (!local_ip.empty()) {
    bind_addr.sin_addr.s_addr = inet_addr(local_ip.c_str());
    if (bind_addr.sin_addr.s_addr == INADDR_NONE) {
      bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
  } else {
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  }

  if (bind(sock, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) == SOCKET_ERROR) {
    closesocket(sock);
    return INVALID_SOCKET;
  }

  return sock;
}

static DiscoveryResult wait_for_discovery_reply(std::vector<SOCKET>& sockets, int timeout_ms) {
  DiscoveryResult result;
  DWORD deadline = GetTickCount() + timeout_ms;

  while (static_cast<int>(GetTickCount() - deadline) < 0) {
    fd_set readfds;
    FD_ZERO(&readfds);

    int max_sockets = 0;
    for (SOCKET sock : sockets) {
      if (sock != INVALID_SOCKET) {
        FD_SET(sock, &readfds);
        ++max_sockets;
      }
    }

    if (max_sockets == 0) {
      break;
    }

    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200 * 1000;

    int ready = select(0, &readfds, nullptr, nullptr, &tv);
    if (ready <= 0) {
      continue;
    }

    for (SOCKET sock : sockets) {
      if (sock == INVALID_SOCKET || !FD_ISSET(sock, &readfds)) {
        continue;
      }

      char buf[128]{};
      sockaddr_in sender{};
      int sender_len = sizeof(sender);
      int received = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                              reinterpret_cast<sockaddr*>(&sender), &sender_len);
      if (received == SOCKET_ERROR || received == 0) {
        continue;
      }

      buf[received] = '\0';
      result.server_ip = ip_to_string(sender);
      result.message = buf;

      if (result.message == kDiscoveryReady) {
        result.status = DiscoveryStatus::Ready;
      } else if (result.message == kDiscoveryBusy) {
        result.status = DiscoveryStatus::Busy;
      } else {
        result.status = DiscoveryStatus::Invalid;
      }

      return result;
    }
  }

  result.status = DiscoveryStatus::NotFound;
  return result;
}

static DiscoveryResult probe_direct(const std::string& server_ip, int port) {
  DiscoveryResult result;

  SOCKET sock = create_discovery_socket("", false);
  if (sock == INVALID_SOCKET) {
    result.status = DiscoveryStatus::Invalid;
    return result;
  }

  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(static_cast<u_short>(port));
  dest.sin_addr.s_addr = inet_addr(server_ip.c_str());
  if (dest.sin_addr.s_addr == INADDR_NONE) {
    closesocket(sock);
    result.status = DiscoveryStatus::Invalid;
    return result;
  }

  if (sendto(sock, kDiscoveryRequest, static_cast<int>(std::strlen(kDiscoveryRequest)), 0,
             reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) == SOCKET_ERROR) {
    closesocket(sock);
    result.status = DiscoveryStatus::Invalid;
    return result;
  }

  std::vector<SOCKET> sockets{sock};
  DiscoveryResult reply = wait_for_discovery_reply(sockets, kDiscoveryTimeoutMs);
  closesocket(sock);
  return reply;
}

static DiscoveryResult probe_broadcast(int port) {
  DiscoveryResult result;
  std::vector<BroadcastTarget> targets = build_broadcast_targets();
  std::vector<SOCKET> sockets;

  for (const BroadcastTarget& target : targets) {
    SOCKET sock = create_discovery_socket(target.local_ip, true);
    if (sock == INVALID_SOCKET) {
      continue;
    }

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(static_cast<u_short>(port));
    dest.sin_addr.s_addr = inet_addr(target.broadcast_ip.c_str());
    if (dest.sin_addr.s_addr == INADDR_NONE) {
      closesocket(sock);
      continue;
    }

    if (sendto(sock, kDiscoveryRequest, static_cast<int>(std::strlen(kDiscoveryRequest)), 0,
               reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) == SOCKET_ERROR) {
      closesocket(sock);
      continue;
    }

    sockets.push_back(sock);
  }

  if (sockets.empty()) {
    result.status = DiscoveryStatus::Invalid;
    return result;
  }

  DiscoveryResult reply = wait_for_discovery_reply(sockets, kDiscoveryTimeoutMs);
  for (SOCKET sock : sockets) {
    closesocket(sock);
  }
  return reply;
}

static SOCKET connect_to(const char* ip, int port, int* wsa_error = nullptr) {
  SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock == INVALID_SOCKET) {
    if (wsa_error) {
      *wsa_error = WSAGetLastError();
    }
    return INVALID_SOCKET;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<u_short>(port));
  addr.sin_addr.s_addr = inet_addr(ip);
  if (addr.sin_addr.s_addr == INADDR_NONE) {
    if (wsa_error) {
      *wsa_error = WSAEINVAL;
    }
    closesocket(sock);
    return INVALID_SOCKET;
  }

  if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    if (wsa_error) {
      *wsa_error = WSAGetLastError();
    }
    closesocket(sock);
    return INVALID_SOCKET;
  }

  if (wsa_error) {
    *wsa_error = 0;
  }
  return sock;
}

class ClientSession {
public:
  explicit ClientSession(SOCKET socket) : socket_(socket) {}

  ~ClientSession() {
    stop();
  }

  bool start() {
    if (!channel_.init_client(socket_)) {
      return false;
    }

    running_.store(true);
    reader_ = std::thread(&ClientSession::reader_loop, this);
    return true;
  }

  bool send_command(const std::string& text) {
    std::vector<uint8_t> payload(text.begin(), text.end());
    return channel_.send_msg(socket_, payload);
  }

  bool wait_for_response(std::string& response) {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    response_cv_.wait(lock, [this] {
      return !responses_.empty() || !running_.load();
    });

    if (responses_.empty()) {
      return false;
    }

    response = std::move(responses_.front());
    responses_.pop();
    return true;
  }

  void stop() {
    stop_requested_.store(true);
    if (socket_ != INVALID_SOCKET) {
      shutdown(socket_, SD_BOTH);
    }
    if (reader_.joinable()) {
      reader_.join();
    }
    if (socket_ != INVALID_SOCKET) {
      closesocket(socket_);
      socket_ = INVALID_SOCKET;
    }
  }

private:
  void reader_loop() {
    while (!stop_requested_.load()) {
      std::vector<uint8_t> plaintext;
      if (!channel_.recv_msg(socket_, plaintext)) {
        if (!stop_requested_.load()) {
          int error = WSAGetLastError();
          if (error == WSAETIMEDOUT) {
            std::cout << "\nServer timed out after 1 minute of inactivity.\n";
          } else {
            std::cout << "\nConnection closed.\n";
          }
          std::cout.flush();
          ExitProcess(0);
        }
        break;
      }

      std::string text(plaintext.begin(), plaintext.end());
      if (text == kTimeoutReply) {
        std::cout << "\nServer timed out after 1 minute of inactivity.\n";
        std::cout.flush();
        ExitProcess(0);
      }

      {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        responses_.push(text);
      }
      response_cv_.notify_one();
    }

    running_.store(false);
    response_cv_.notify_all();
  }

  SOCKET socket_ = INVALID_SOCKET;
  CryptoChannel channel_;
  std::thread reader_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::mutex queue_mutex_;
  std::condition_variable response_cv_;
  std::queue<std::string> responses_;
};

static void print_welcome() {
  std::cout << "\x1b[38;2;255;232;31m";
  std::cout << R"SWAPSTER_BANNER(
================================================================================================
     _______.____    __    ____  ___      .______     _______.___________. _______ .______
    /       |\   \  /  \  /   / /   \     |   _  \   /       |           ||   ____||   _  \
   |   (----` \   \/    \/   / /  ^  \    |  |_)  | |   (----`---|  |----`|  |__   |  |_)  |
    \   \      \            / /  /_\  \   |   ___/   \   \       |  |     |   __|  |      /
.----)   |      \    /\    / /  _____  \  |  |   .----)   |      |  |     |  |____ |  |\  \----
|_______/        \__/  \__/ /__/     \__\ | _|   |_______/       |__|     |_______|| _| `._____|

================================================================================================
Encrypted session established.
Type commands and press Enter.
SWAP: Swap the contents of the displays in the target computer
EXIT: Disconnect from target computer and exit controller
TERM: Stop the server on the target computer
================================================================================================
)SWAPSTER_BANNER";
  std::cout << "\x1b[0m";
  std::cout.flush();
}

} // namespace

int main(int argc, char** argv) {
  enable_ansi_colors();

  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    std::cerr << "WSAStartup failed\n";
    return 1;
  }

  std::string server_ip;
  std::string requested_ip;
  int port = 0;
  DiscoveryResult discovery;
  bool direct_mode = false;

  if (argc == 3) {
    requested_ip = argv[1];
    server_ip = requested_ip;
    port = std::atoi(argv[2]);
    std::cout << "Connecting to " << server_ip << ":" << port << "...\n" << std::endl;
    discovery = probe_direct(server_ip, port);
    direct_mode = true;
  } else if (argc == 1) {
    std::cout << "Searching for Swapster computer on LAN...\n" << std::endl;
    port = 2003;
    discovery = probe_broadcast(port);
  } else {
    std::cerr << "Usage: controller.exe [ip port]\n";
    std::cerr << "       controller.exe (for auto-discovery)\n";
    WSACleanup();
    return 1;
  }

  if (discovery.status == DiscoveryStatus::Busy) {
    std::cout << "Swapster server at " << discovery.server_ip << ":" << port << " is busy.\n";
    WSACleanup();
    return 1;
  }

  if (discovery.status != DiscoveryStatus::Ready) {
    std::cerr << "Could not find a ready Swapster server\n";
    WSACleanup();
    return 1;
  }

  if (!direct_mode) {
    server_ip = discovery.server_ip;
  }
  std::cout << "\n*** Found Swapster server at " << server_ip << ":" << port << " ***\n" << std::endl;
  std::cout << "\nConnecting to " << server_ip << ":" << port << "...\n" << std::endl;

  SOCKET socket = INVALID_SOCKET;
  int last_wsa_error = 0;
  const int max_attempts = 5;

  for (int attempt = 1; attempt <= max_attempts; ++attempt) {
    socket = connect_to(server_ip.c_str(), port, &last_wsa_error);
    if (socket == INVALID_SOCKET) {
      if (attempt < max_attempts) {
        std::cerr << "Connect attempt " << attempt << " failed (WSA " << last_wsa_error
                  << "), retrying...\n";
        Sleep(200 * attempt);
      }
      continue;
    }

    ClientSession session(socket);
    if (session.start()) {
      print_welcome();

      std::string line;
      while (std::getline(std::cin, line)) {
        if (line == "EXIT") {
          break;
        }

        if (line == "TERM") {
          std::cout << "Are you sure you want to kill swapster? Type 'YES' to confirm: ";
          std::string confirm;
          std::getline(std::cin, confirm);
          if (confirm != "YES") {
            std::cout << "Termination cancelled.\n";
            continue;
          }
        }

        if (!session.send_command(line)) {
          std::cerr << "Send failed\n";
          break;
        }

        std::string response;
        if (!session.wait_for_response(response)) {
          std::cerr << "Receive failed\n";
          break;
        }

        std::cout << response << "\n";

        if (line == "TERM") {
          std::cout << "Termination command acknowledged. Exiting controller.\n";
          break;
        }
      }

      session.stop();
      WSACleanup();
      return 0;
    }

    closesocket(socket);
    socket = INVALID_SOCKET;

    if (attempt < max_attempts) {
      std::cerr << "Handshake attempt " << attempt << " failed, retrying...\n";
      Sleep(200 * attempt);
    }
  }

  std::cerr << "Connect failed after " << max_attempts << " attempts";
  if (last_wsa_error != 0) {
    std::cerr << " (last WSA " << last_wsa_error << ")";
  }
  std::cerr << "\n";

  WSACleanup();
  return 1;
}
