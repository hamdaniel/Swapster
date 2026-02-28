#pragma once

#include <string>
#include <vector>
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>

#pragma comment(lib, "iphlpapi.lib")

namespace lan {

/// Get the subnet of the default adapter (e.g., "192.168.1.0")
std::string GetSubnetAddress();

/// Get the subnet mask of the default adapter (e.g., "255.255.255.0")
std::string GetSubnetMask();

/// Get the local IP address of the default adapter (e.g., "192.168.1.100")
std::string GetLocalIP();

/// Discover Swapster server via UDP broadcast on specified port
/// Returns server IP if found, empty string otherwise
/// timeout_ms: how long to wait for responses (default 2000ms)
std::string DiscoverServerUDP(int port = 2003, int timeout_ms = 2000);

} // namespace lan
