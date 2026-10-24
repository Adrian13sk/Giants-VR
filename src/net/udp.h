// A small non-blocking UDP socket (POSIX: Linux host and Android). For the local-network game
// (game/sk_net.h): datagrams to an address, broadcast on the LAN, and polling for what arrived.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gvr {

struct NetAddr {
    uint32_t ip = 0;     // host byte order (0x7F000001 = 127.0.0.1)
    uint16_t port = 0;
    bool operator==(const NetAddr& o) const { return ip == o.ip && port == o.port; }
    bool operator!=(const NetAddr& o) const { return !(*this == o); }
    std::string str() const;
    static bool parse(const std::string& s, uint16_t defaultPort, NetAddr& out);  // "a.b.c.d[:port]"
    static NetAddr broadcast(uint16_t port) { return {0xFFFFFFFFu, port}; }
};

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket() { close(); }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    // Binds to `port` on every interface (0: any free port); broadcast sending allowed.
    bool open(uint16_t port);
    void close();
    bool isOpen() const { return fd_ >= 0; }
    uint16_t localPort() const { return port_; }
    bool send(const NetAddr& to, const void* data, size_t n);
    // One datagram if any arrived (non-blocking); false when none.
    bool recv(std::vector<uint8_t>& out, NetAddr& from);
    // The machine's IPv4 addresses (not loopback), for showing "host at ...".
    static std::vector<NetAddr> localAddresses();

private:
    int fd_ = -1;
    uint16_t port_ = 0;
};

}  // namespace gvr
