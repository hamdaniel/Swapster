#pragma once

#include <string>
#include <vector>
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>

#pragma comment(lib, "iphlpapi.lib")

namespace network_map {

/// Get the subnet of the default adapter (e.g., "192.168.1.0")
std::string GetSubnetAddress();

/// Get the subnet mask of the default adapter (e.g., "255.255.255.0")
std::string GetSubnetMask();

/// Get the local IP address of the default adapter (e.g., "192.168.1.100")
std::string GetLocalIP();

/// Ping all IPs in the current subnet to populate the ARP table
void PingSubnet();

/// Get list of all active IPv4 addresses on the LAN (via ARP table)
std::vector<std::string> GetActiveIPs();

/// Send a string to a remote host via TCP, receive first 16 bytes back
/// Returns received data (empty string on failure)
std::string SendString(const std::string& ip, int port, const std::string& data);

} // namespace network_map
