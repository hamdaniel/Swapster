#pragma once

#include <string>
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>

#pragma comment(lib, "iphlpapi.lib")

namespace lan {

/// Discover Swapster server via UDP broadcast on specified port
/// Returns server IP if found, empty string otherwise
/// Tries all network adapters, prioritizing real adapters (with gateways) over virtual ones
/// timeout_ms: how long to wait for responses (default 2000ms)
std::string DiscoverServerUDP(int port = 2003, int timeout_ms = 2000);

} // namespace lan
