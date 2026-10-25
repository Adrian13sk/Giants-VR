#include "net/udp.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace gvr {

std::string NetAddr::str() const {
    char b[32];
    std::snprintf(b, sizeof b, "%u.%u.%u.%u:%u", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port);
    return b;
}

bool NetAddr::parse(const std::string& s, uint16_t defaultPort, NetAddr& out) {
    unsigned a, b, c, d, p = defaultPort;
    const int n = std::sscanf(s.c_str(), "%u.%u.%u.%u:%u", &a, &b, &c, &d, &p);
    if (n < 4 || a > 255 || b > 255 || c > 255 || d > 255 || p > 65535) return false;
    out.ip = (a << 24) | (b << 16) | (c << 8) | d;
    out.port = uint16_t(p);
    return true;
}

bool UdpSocket::open(uint16_t port) {
    close();
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return false;
    int on = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { close(); return false; }
    socklen_t len = sizeof a;
    ::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
    port_ = ntohs(a.sin_port);
    ::fcntl(fd_, F_SETFL, ::fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
    return true;
}

void UdpSocket::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    port_ = 0;
}

bool UdpSocket::send(const NetAddr& to, const void* data, size_t n) {
    if (fd_ < 0) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(to.ip);
    a.sin_port = htons(to.port);
    return ::sendto(fd_, data, n, 0, reinterpret_cast<sockaddr*>(&a), sizeof a) == ssize_t(n);
}

bool UdpSocket::recv(std::vector<uint8_t>& out, NetAddr& from) {
    if (fd_ < 0) return false;
    uint8_t buf[4096];  // (a snapshot is kept under 1400 bytes; room for a larger one all the same)
    sockaddr_in a{};
    socklen_t len = sizeof a;
    const ssize_t n = ::recvfrom(fd_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&a), &len);
    if (n <= 0) return false;
    out.assign(buf, buf + n);
    from.ip = ntohl(a.sin_addr.s_addr);
    from.port = ntohs(a.sin_port);
    return true;
}

std::vector<NetAddr> UdpSocket::localAddresses() {
    std::vector<NetAddr> out;
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return out;
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
        const uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
        out.push_back({ip, 0});
    }
    ::freeifaddrs(list);
    return out;
}

}  // namespace gvr
