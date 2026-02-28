#include "lan.h"
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <winerror.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <set>
#include <iostream>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace lan {

// Helper: convert IPv4 address bytes to dotted string
static std::string IPToString(unsigned char a, unsigned char b, unsigned char c, unsigned char d) {
    char buf[20];
    sprintf_s(buf, sizeof(buf), "%d.%d.%d.%d", a, b, c, d);
    return std::string(buf);
}

// Helper: parse dotted IP string to 4 bytes
static bool StringToIP(const std::string& s, unsigned char& a, unsigned char& b, unsigned char& c, unsigned char& d) {
    return (sscanf_s(s.c_str(), "%hhu.%hhu.%hhu.%hhu", &a, &b, &c, &d) == 4);
}

// Helper: get default adapter info
static bool GetDefaultAdapterInfo(PIP_ADAPTER_INFO& adapter) {
    ULONG size = 0;
    if (GetAdaptersInfo(NULL, &size) == ERROR_BUFFER_OVERFLOW) {
        PIP_ADAPTER_INFO pAdapterInfo = (IP_ADAPTER_INFO*)malloc(size);
        if (!pAdapterInfo) return false;
        
        if (GetAdaptersInfo(pAdapterInfo, &size) == NO_ERROR) {
            adapter = pAdapterInfo;
            return true;
        }
        free(pAdapterInfo);
    }
    return false;
}

std::string GetSubnetAddress() {
    PIP_ADAPTER_INFO adapter = NULL;
    if (!GetDefaultAdapterInfo(adapter)) return "";
    
    unsigned char a, b, c, d, ma, mb, mc, md;
    if (!StringToIP(adapter->IpAddressList.IpAddress.String, a, b, c, d) ||
        !StringToIP(adapter->IpAddressList.IpMask.String, ma, mb, mc, md)) {
        free(adapter);
        return "";
    }
    
    unsigned char sa = a & ma;
    unsigned char sb = b & mb;
    unsigned char sc = c & mc;
    unsigned char sd = d & md;
    
    std::string result = IPToString(sa, sb, sc, sd);
    free(adapter);
    return result;
}

std::string GetSubnetMask() {
    PIP_ADAPTER_INFO adapter = NULL;
    if (!GetDefaultAdapterInfo(adapter)) return "";
    
    std::string result(adapter->IpAddressList.IpMask.String);
    free(adapter);
    return result;
}

std::string GetLocalIP() {
    ULONG size = 0;
    if (GetAdaptersInfo(NULL, &size) == ERROR_BUFFER_OVERFLOW) {
        PIP_ADAPTER_INFO pAdapterInfo = (IP_ADAPTER_INFO*)malloc(size);
        if (!pAdapterInfo) return "";
        
        std::string fallback_ip = "";
        
        if (GetAdaptersInfo(pAdapterInfo, &size) == NO_ERROR) {
            // First pass: prefer adapters with a gateway (real adapters, not virtual)
            for (PIP_ADAPTER_INFO adapter = pAdapterInfo; adapter != NULL; adapter = adapter->Next) {
                // Check if adapter has a gateway
                if (adapter->GatewayList.IpAddress.String[0] != '\0') {
                    for (PIP_ADDR_STRING ip_addr = &adapter->IpAddressList; ip_addr != NULL; ip_addr = ip_addr->Next) {
                        std::string ip(ip_addr->IpAddress.String);
                        
                        if (!ip.empty() && ip != "0.0.0.0" && ip.find("127.") != 0) {
                            free(pAdapterInfo);
                            return ip;
                        }
                    }
                }
            }
            
            // Second pass: fallback to any valid IP
            for (PIP_ADAPTER_INFO adapter = pAdapterInfo; adapter != NULL; adapter = adapter->Next) {
                for (PIP_ADDR_STRING ip_addr = &adapter->IpAddressList; ip_addr != NULL; ip_addr = ip_addr->Next) {
                    std::string ip(ip_addr->IpAddress.String);
                    
                    if (!ip.empty() && ip != "0.0.0.0" && ip.find("127.") != 0) {
                        fallback_ip = ip;
                    }
                }
            }
        }
        free(pAdapterInfo);
        return fallback_ip;
    }
    return "";
}

void PingSubnet() {
    std::string subnet = GetSubnetAddress();
    if (subnet.empty()) {
        std::cerr << "Failed to get subnet address\n";
        return;
    }
    
    unsigned char a, b, c, d;
    if (!StringToIP(subnet, a, b, c, d)) return;
    
    std::cout << "Priming ARP table by pinging subnet..." << std::endl;
    
    // Hide cursor during progress bar
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO cursorInfo;
    GetConsoleCursorInfo(hConsole, &cursorInfo);
    cursorInfo.bVisible = FALSE;
    SetConsoleCursorInfo(hConsole, &cursorInfo);
    
    // Ping all IPs in the subnet (1-254, skipping 0 and 255)
    for (int i = 1; i < 255; i++) {
        char ip[20];
        sprintf_s(ip, sizeof(ip), "%d.%d.%d.%d", a, b, c, i);
        
        std::string cmd = "ping -n 1 -w 75 ";
        cmd += ip;
        cmd += " >nul 2>&1";
        
        system(cmd.c_str());
        
        // Show progress bar every few iterations
        if (i % 10 == 0 || i == 254) {
            int percent = (int)(i * 100.0 / 254.0);
            int bar_width = 50;
            int filled = (int)(bar_width * percent / 100.0);
            
            std::cout << "\r[";
            for (int j = 0; j < bar_width; ++j) {
                std::cout << (j < filled ? "=" : " ");
            }
            std::cout << "] " << percent << "%";
            std::cout.flush();
        }
    }
    std::cout << std::endl;
    
    // Restore cursor
    cursorInfo.bVisible = TRUE;
    SetConsoleCursorInfo(hConsole, &cursorInfo);
}

std::vector<std::string> GetActiveIPs() {
    std::set<std::string> ips;
    
    // Add the local IP first
    std::string local_ip = GetLocalIP();
    if (!local_ip.empty()) {
        ips.insert(local_ip);
    }
    
    // Get the ARP table size
    DWORD dwSize = 0;
    if (GetIpNetTable(NULL, &dwSize, FALSE) == ERROR_INSUFFICIENT_BUFFER) {
        PMIB_IPNETTABLE pIpNetTable = (PMIB_IPNETTABLE)malloc(dwSize);
        if (!pIpNetTable) return std::vector<std::string>();
        
        if (GetIpNetTable(pIpNetTable, &dwSize, FALSE) == NO_ERROR) {
            for (DWORD i = 0; i < pIpNetTable->dwNumEntries; i++) {
                MIB_IPNETROW& row = pIpNetTable->table[i];
                
                // Only include ARP entries that are valid and reachable
                if (row.dwType == MIB_IPNET_TYPE_DYNAMIC || row.dwType == MIB_IPNET_TYPE_STATIC) {
                    unsigned char* ip = (unsigned char*)&row.dwAddr;
                    std::string ipStr = IPToString(ip[0], ip[1], ip[2], ip[3]);
                    ips.insert(ipStr);
                }
            }
        }
        
        free(pIpNetTable);
    }
    
    std::vector<std::string> result(ips.begin(), ips.end());
    
    // Print table
    std::cout << "\n+-------------------+" << std::endl;
    std::cout << "|   Active IPs      |" << std::endl;
    std::cout << "+-------------------+" << std::endl;
    for (const auto& ip : result) {
        std::cout << "| " << ip;
        // Pad to 15 chars (max IPv4 length)
        for (size_t i = ip.length(); i < 15; i++) std::cout << " ";
        std::cout << " |" << std::endl;
    }
    std::cout << "+-------------------+" << std::endl;
    std::cout << std::endl;
    
    return result;
}

std::string DiscoverServerUDP(int port, int timeout_ms) {
    // Initialize Winsock if needed
    WSADATA wsaData;
    static bool wsa_initialized = false;
    if (!wsa_initialized) {
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            return "";
        }
        wsa_initialized = true;
    }
    
    // Collect all adapters
    ULONG size = 0;
    if (GetAdaptersInfo(NULL, &size) != ERROR_BUFFER_OVERFLOW) {
        return "";
    }
    
    PIP_ADAPTER_INFO pAdapterInfo = (IP_ADAPTER_INFO*)malloc(size);
    if (!pAdapterInfo) return "";
    
    if (GetAdaptersInfo(pAdapterInfo, &size) != NO_ERROR) {
        free(pAdapterInfo);
        return "";
    }
    
    std::string result = "";
    
    // First pass: try adapters WITH gateways (real adapters)
    for (PIP_ADAPTER_INFO adapter = pAdapterInfo; adapter != NULL; adapter = adapter->Next) {
        if (adapter->GatewayList.IpAddress.String[0] == '\0') continue; // skip no gateway
        
        for (PIP_ADDR_STRING ip_addr = &adapter->IpAddressList; ip_addr != NULL; ip_addr = ip_addr->Next) {
            std::string local_ip(ip_addr->IpAddress.String);
            std::string mask(adapter->IpAddressList.IpMask.String);
            
            if (local_ip.empty() || local_ip == "0.0.0.0" || local_ip.find("127.") == 0) continue;
            
            // Calculate broadcast
            unsigned char subnet[4], subnet_mask[4], broadcast[4];
            if (!StringToIP(local_ip, subnet[0], subnet[1], subnet[2], subnet[3]) ||
                !StringToIP(mask, subnet_mask[0], subnet_mask[1], subnet_mask[2], subnet_mask[3])) {
                continue;
            }
            
            // Apply mask to get subnet, then OR with ~mask to get broadcast
            for (int i = 0; i < 4; i++) {
                subnet[i] = subnet[i] & subnet_mask[i];
                broadcast[i] = subnet[i] | (~subnet_mask[i]);
            }
            
            std::string broadcast_ip = IPToString(broadcast[0], broadcast[1], broadcast[2], broadcast[3]);
            
            std::cout << "Trying adapter " << local_ip << " -> broadcast to " << broadcast_ip << "\n";
            
            // Try this adapter
            SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock == INVALID_SOCKET) continue;
            
            BOOL broadcast_enabled = TRUE;
            setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcast_enabled, sizeof(broadcast_enabled));
            
            DWORD timeout = timeout_ms;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
            
            sockaddr_in localAddr{};
            localAddr.sin_family = AF_INET;
            localAddr.sin_port = 0;
            localAddr.sin_addr.s_addr = INADDR_ANY;
            
            if (bind(sock, (sockaddr*)&localAddr, sizeof(localAddr)) == SOCKET_ERROR) {
                closesocket(sock);
                continue;
            }
            
            sockaddr_in broadcastAddr{};
            broadcastAddr.sin_family = AF_INET;
            broadcastAddr.sin_port = htons((u_short)port);
            broadcastAddr.sin_addr.s_addr = inet_addr(broadcast_ip.c_str());
            
            const char* discovery_msg = "SWAPSTER_DISCOVER";
            if (sendto(sock, discovery_msg, (int)strlen(discovery_msg), 0, 
                       (sockaddr*)&broadcastAddr, sizeof(broadcastAddr)) != SOCKET_ERROR) {
                
                char buf[64];
                sockaddr_in senderAddr;
                int senderAddrSize = sizeof(senderAddr);
                
                int received = recvfrom(sock, buf, sizeof(buf) - 1, 0, 
                                        (sockaddr*)&senderAddr, &senderAddrSize);
                
                if (received > 0) {
                    buf[received] = '\0';
                    if (strcmp(buf, "SWAPSTER_HERE") == 0) {
                        char* ip = inet_ntoa(senderAddr.sin_addr);
                        result = std::string(ip);
                        closesocket(sock);
                        free(pAdapterInfo);
                        return result;
                    }
                }
            }
            
            closesocket(sock);
        }
    }
    
    // Second pass: try adapters WITHOUT gateways (virtual adapters)
    for (PIP_ADAPTER_INFO adapter = pAdapterInfo; adapter != NULL; adapter = adapter->Next) {
        if (adapter->GatewayList.IpAddress.String[0] != '\0') continue; // skip ones with gateway
        
        for (PIP_ADDR_STRING ip_addr = &adapter->IpAddressList; ip_addr != NULL; ip_addr = ip_addr->Next) {
            std::string local_ip(ip_addr->IpAddress.String);
            std::string mask(adapter->IpAddressList.IpMask.String);
            
            if (local_ip.empty() || local_ip == "0.0.0.0" || local_ip.find("127.") == 0) continue;
            
            // Calculate broadcast
            unsigned char subnet[4], subnet_mask[4], broadcast[4];
            if (!StringToIP(local_ip, subnet[0], subnet[1], subnet[2], subnet[3]) ||
                !StringToIP(mask, subnet_mask[0], subnet_mask[1], subnet_mask[2], subnet_mask[3])) {
                continue;
            }
            
            for (int i = 0; i < 4; i++) {
                subnet[i] = subnet[i] & subnet_mask[i];
                broadcast[i] = subnet[i] | (~subnet_mask[i]);
            }
            
            std::string broadcast_ip = IPToString(broadcast[0], broadcast[1], broadcast[2], broadcast[3]);
            
            std::cout << "Trying virtual adapter " << local_ip << " -> broadcast to " << broadcast_ip << "\n";
            
            // Try this adapter
            SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock == INVALID_SOCKET) continue;
            
            BOOL broadcast_enabled = TRUE;
            setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcast_enabled, sizeof(broadcast_enabled));
            
            DWORD timeout = timeout_ms;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
            
            sockaddr_in localAddr{};
            localAddr.sin_family = AF_INET;
            localAddr.sin_port = 0;
            localAddr.sin_addr.s_addr = INADDR_ANY;
            
            if (bind(sock, (sockaddr*)&localAddr, sizeof(localAddr)) == SOCKET_ERROR) {
                closesocket(sock);
                continue;
            }
            
            sockaddr_in broadcastAddr{};
            broadcastAddr.sin_family = AF_INET;
            broadcastAddr.sin_port = htons((u_short)port);
            broadcastAddr.sin_addr.s_addr = inet_addr(broadcast_ip.c_str());
            
            const char* discovery_msg = "SWAPSTER_DISCOVER";
            if (sendto(sock, discovery_msg, (int)strlen(discovery_msg), 0, 
                       (sockaddr*)&broadcastAddr, sizeof(broadcastAddr)) != SOCKET_ERROR) {
                
                char buf[64];
                sockaddr_in senderAddr;
                int senderAddrSize = sizeof(senderAddr);
                
                int received = recvfrom(sock, buf, sizeof(buf) - 1, 0, 
                                        (sockaddr*)&senderAddr, &senderAddrSize);
                
                if (received > 0) {
                    buf[received] = '\0';
                    if (strcmp(buf, "SWAPSTER_HERE") == 0) {
                        char* ip = inet_ntoa(senderAddr.sin_addr);
                        result = std::string(ip);
                        closesocket(sock);
                        free(pAdapterInfo);
                        return result;
                    }
                }
            }
            
            closesocket(sock);
        }
    }
    
    free(pAdapterInfo);
    return "";
}

} // namespace lan
