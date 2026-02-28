#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>

#include <iostream>
#include <string>
#include <vector>
#include "encryption.h"
#include "network_map.h"

#pragma comment(lib, "ws2_32.lib")


// Find Swapster server by scanning LAN
// Returns pair<IP, port> if found, or pair<"", -1> if not found
std::pair<std::string, int> FindSwapsterServer(int port_start = 2003, int port_end = 2003) {
  network_map::PingSubnet();
  
  std::cout << "Retrieving active IPs from ARP table..." << std::endl;
  std::vector<std::string> ips = network_map::GetActiveIPs();
  
  int total_ips = ips.size();
  
  // Hide cursor during progress bar
  HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
  CONSOLE_CURSOR_INFO cursorInfo;
  GetConsoleCursorInfo(hConsole, &cursorInfo);
  cursorInfo.bVisible = FALSE;
  SetConsoleCursorInfo(hConsole, &cursorInfo);
  
  std::cout << "Scanning for Swapster server..." << std::endl;
  for (int idx = 0; idx < total_ips; ++idx) {
    const auto& ip = ips[idx];
    
    // Show progress bar
    int percent = (int)((idx + 1) * 100.0 / total_ips);
    int bar_width = 50;
    int filled = (int)(bar_width * percent / 100.0);
    
    std::cout << "\r[";
    for (int i = 0; i < bar_width; ++i) {
      std::cout << (i < filled ? "=" : " ");
    }
    std::cout << "] " << percent << "% [" << ip << "]";
    // Pad with spaces to clear previous longer IP
    for (int i = 0; i < 20; ++i) std::cout << " ";
    std::cout.flush();
    
    for (int port = port_start; port <= port_end; ++port) {
      std::string response = network_map::SendString(ip, port, "swapsterswapster");
      if (response == "SwapsterServerOK") {
        // Restore cursor
        cursorInfo.bVisible = TRUE;
        SetConsoleCursorInfo(hConsole, &cursorInfo);
        
        std::cout << "\n\n*** Found Swapster server at " << ip << ":" << port << " ***\n" << std::endl;
        return {ip, port};
      }
    }
  }
  
  // Restore cursor
  cursorInfo.bVisible = TRUE;
  SetConsoleCursorInfo(hConsole, &cursorInfo);
  
  std::cout << "\n\nSwapster server not found on network" << std::endl;
  return {"", -1};
}

void print_welcome() {
  std::cout << "\x1b[38;2;255;232;31m";
  std::cout << R"(
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
  EXIT: Disconnect from server
  TERM: Kill the server process on the target computer 
================================================================================================
)";
  std::cout << "\x1b[0m";
}

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
  
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
    std::cerr << "WSAStartup failed\n";
    return 1;
  }

  std::string ip_str;
  int port;
  
  if (argc == 3) {
    // Use provided IP and port
    ip_str = argv[1];
    port = std::atoi(argv[2]);
    std::cout << "Connecting to " << ip_str << ":" << port << "...\n" << std::endl;
  } else if (argc == 1) {
    // Auto-discover
    std::cout << "Searching for Swapster server on LAN...\n" << std::endl;
    auto [ip, p] = FindSwapsterServer();
    
    if (p == -1) {
      std::cerr << "Could not find Swapster server on network\n";
      WSACleanup();
      return 1;
    }
    
    ip_str = ip;
    port = p;
    std::cout << "\nConnecting to " << ip_str << ":" << port << "...\n" << std::endl;
  } else {
    std::cerr << "Usage: client.exe [ip port]\n";
    std::cerr << "       client.exe (for auto-discovery)\n";
    WSACleanup();
    return 1;
  }
  
  SOCKET s = connect_to(ip_str.c_str(), port);
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

  print_welcome();

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "EXIT") break;
    if (line == "TERM") {
      std::cout << "Are you sure you want to kill the server process? Type 'YES' to confirm: ";
      std::string confirm;
      std::getline(std::cin, confirm);
      if (confirm == "YES") {
        line = "TERM";
      } else {
        std::cout << "Termination cancelled.\n";
        continue;
      }
    }
    std::vector<uint8_t> pt(line.begin(), line.end());

    if (!ch.send_msg(s, pt)) {
      std::cerr << "Send failed\n";
      break;
    }
    if(line == "TERM") {
      std::cout << "Termination command sent. Exiting client.\n";
      break;
    }
    std::vector<uint8_t> echo_pt;
    if (!ch.recv_msg(s, echo_pt)) {
      std::cerr << "Receive failed\n";
      break;
    }

    std::cout << std::string(echo_pt.begin(), echo_pt.end()) << "\n";
  }

  closesocket(s);
  WSACleanup();
  return 0;
}