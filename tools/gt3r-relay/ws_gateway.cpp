#include "ws_gateway.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#ifndef MSG_NOSIGNAL // (macOS: SO_NOSIGPIPE on each connection, and main() ignores SIGPIPE)
#define MSG_NOSIGNAL 0
#endif
#endif

namespace gt3r::net {

#if defined(_WIN32)

struct WsGateway::Impl {};
WsGateway::WsGateway() = default;
WsGateway::~WsGateway() = default;
bool WsGateway::start(const Options&, std::function<void(const std::string&)>, std::string* error) {
    if (error) *error = "the browsers' WebSocket port runs on Linux and macOS only";
    return false;
}
void WsGateway::stop() {}
WsGateway::Stats WsGateway::stats() const { return {}; }

#else

namespace {

// --- the WebSocket handshake: SHA-1 and base64 (RFC 6455 section 4.2.2) ---

u32 rol(u32 v, int n) { return (v << n) | (v >> (32 - n)); }

void sha1(const std::string& text, u8 out[20]) {
    u32 h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string m = text;
    const u64 bits = u64(text.size()) * 8;
    m += char(0x80);
    while (m.size() % 64 != 56) m += char(0);
    for (int i = 7; i >= 0; --i) m += char(u8(bits >> (i * 8)));
    for (size_t chunk = 0; chunk < m.size(); chunk += 64) {
        u32 w[80];
        for (int i = 0; i < 16; ++i) {
            const u8* p = reinterpret_cast<const u8*>(m.data()) + chunk + i * 4;
            w[i] = u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | u32(p[3]);
        }
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            u32 f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            const u32 t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = u8(h[i] >> (24 - j * 8));
}

std::string base64(const u8* p, size_t n) {
    static const char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    for (size_t i = 0; i < n; i += 3) {
        const u32 v = u32(p[i]) << 16 | (i + 1 < n ? u32(p[i + 1]) << 8 : 0) | (i + 2 < n ? u32(p[i + 2]) : 0);
        s += kChars[v >> 18 & 63];
        s += kChars[v >> 12 & 63];
        s += i + 1 < n ? kChars[v >> 6 & 63] : '=';
        s += i + 2 < n ? kChars[v & 63] : '=';
    }
    return s;
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// --- addresses ---

// An IPv4 peer that arrived on a dual-stack socket (::ffff:a.b.c.d) as plain IPv4, so it compares
// equal to the same peer seen elsewhere.
Address normalized(const Address& a) {
    if (a.family() != AF_INET6) return a;
    const auto* s6 = reinterpret_cast<const sockaddr_in6*>(a.storage);
    const u8* b = reinterpret_cast<const u8*>(&s6->sin6_addr);
    static const u8 kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (std::memcmp(b, kMapped, 12) != 0) return a;
    Address v4;
    auto* s4 = reinterpret_cast<sockaddr_in*>(v4.storage);
    s4->sin_family = AF_INET;
    s4->sin_port = s6->sin6_port;
    std::memcpy(&s4->sin_addr, b + 12, 4);
    v4.length = sizeof(sockaddr_in);
    return v4;
}

bool private_v4(const Address& a) {
    if (a.family() != AF_INET) return false;
    const u8* b = reinterpret_cast<const u8*>(&reinterpret_cast<const sockaddr_in*>(a.storage)->sin_addr);
    return b[0] == 10 || (b[0] == 172 && (b[1] & 0xF0) == 16) || (b[0] == 192 && b[1] == 168);
}

// The family byte, address and port of a message (the relay: the family byte 0 alone).
void put_address(std::string& m, const Address& a, bool relay) {
    if (relay) {
        m += char(0);
        return;
    }
    const u16 port = a.port();
    if (a.family() == AF_INET) {
        m += char(4);
        m.append(reinterpret_cast<const char*>(&reinterpret_cast<const sockaddr_in*>(a.storage)->sin_addr), 4);
    } else {
        m += char(6);
        m.append(reinterpret_cast<const char*>(&reinterpret_cast<const sockaddr_in6*>(a.storage)->sin6_addr), 16);
    }
    m += char(port >> 8);
    m += char(port & 0xFF);
}

// The address of a message from a browser; `used` gets the bytes it took. False: malformed.
bool get_address(const u8* p, size_t n, const Address& relay, Address& out, size_t& used) {
    if (n < 1) return false;
    if (p[0] == 0) {
        out = relay;
        used = 1;
        return true;
    }
    const size_t ip = p[0] == 4 ? 4 : p[0] == 6 ? 16 : 0;
    if (!ip || n < 1 + ip + 2) return false;
    out = Address{};
    const u16 port = u16(p[1 + ip] << 8 | p[2 + ip]);
    if (ip == 4) {
        auto* s = reinterpret_cast<sockaddr_in*>(out.storage);
        s->sin_family = AF_INET;
        s->sin_port = htons(port);
        std::memcpy(&s->sin_addr, p + 1, 4);
        out.length = sizeof(sockaddr_in);
    } else {
        auto* s = reinterpret_cast<sockaddr_in6*>(out.storage);
        s->sin6_family = AF_INET6;
        s->sin6_port = htons(port);
        std::memcpy(&s->sin6_addr, p + 1, 16);
        out.length = sizeof(sockaddr_in6);
    }
    used = 1 + ip + 2;
    out = normalized(out);
    return port != 0;
}

void nonblocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }

constexpr size_t kMaxMessage = 2048;      // a datagram and its address (the game's are well under 1500 bytes)
constexpr size_t kMaxHandshake = 8192;
constexpr size_t kMaxBacklog = 256 * 1024; // bytes waiting for a slow browser: further datagrams are dropped, as UDP would
constexpr size_t kMaxAllowed = 64;         // addresses a connection has heard from
constexpr double kIdleMs = 120000, kHandshakeMs = 10000;

} // namespace

struct WsGateway::Impl {
    Options opt;
    std::function<void(const std::string&)> log;
    int listen_fd = -1;
    std::thread thread;
    std::atomic<bool> stopping{false};
    mutable std::mutex stats_mu;
    Stats stats;

    struct Client {
        int tcp = -1, udp = -1;
        bool open = false, closing = false;
        std::string in, out, who;
        double since = 0, last = 0, tokens = 0, token_time = 0;
        std::vector<Address> heard; // sources this connection may send back to
    };
    std::vector<Client> clients;

    void say(const std::string& s) {
        if (log) log("ws: " + s);
    }
    void count(unsigned long long Stats::*field) {
        std::lock_guard<std::mutex> lock(stats_mu);
        ++(stats.*field);
    }

    static void frame(std::string& out, int opcode, const char* data, size_t n) {
        out += char(0x80 | opcode);
        if (n < 126) {
            out += char(n);
        } else if (n < 65536) {
            out += char(126);
            out += char(n >> 8);
            out += char(n & 0xFF);
        } else {
            out += char(127);
            for (int i = 7; i >= 0; --i) out += char(u8(u64(n) >> (i * 8)));
        }
        out.append(data, n);
    }

    void close_client(Client& c, const char* why) {
        if (c.tcp >= 0) ::close(c.tcp);
        if (c.udp >= 0) ::close(c.udp);
        c.tcp = c.udp = -1;
        if (c.open) say("browser left (" + std::string(why) + (c.who.empty() ? "" : ", " + c.who) + ")");
    }

    bool handshake(Client& c, double now) {
        const size_t end = c.in.find("\r\n\r\n");
        if (end == std::string::npos) return c.in.size() <= kMaxHandshake;
        std::string head = c.in.substr(0, end);
        c.in.erase(0, end + 4);
        std::string key, protocol, upgrade;
        size_t pos = head.find("\r\n");
        const std::string request = head.substr(0, pos);
        while (pos != std::string::npos) {
            const size_t next = head.find("\r\n", pos + 2);
            const std::string h = head.substr(pos + 2, next == std::string::npos ? std::string::npos : next - pos - 2);
            pos = next;
            const size_t colon = h.find(':');
            if (colon == std::string::npos) continue;
            const std::string name = lower(trim(h.substr(0, colon))), value = trim(h.substr(colon + 1));
            if (name == "sec-websocket-key") key = value;
            else if (name == "sec-websocket-protocol") protocol = value;
            else if (name == "upgrade") upgrade = lower(value);
        }
        if (request.rfind("GET ", 0) != 0 || upgrade != "websocket" || key.empty()) {
            c.out += "HTTP/1.1 426 Upgrade Required\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
                     "GT3 Revisited relay: the port for the browser build's WebSocket.\n";
            c.closing = true;
            return true;
        }
        u8 digest[20];
        sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", digest);
        c.out += "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                 base64(digest, 20) + "\r\n";
        if (!protocol.empty()) c.out += "Sec-WebSocket-Protocol: " + protocol.substr(0, protocol.find(',')) + "\r\n";
        c.out += "\r\n";
        // Its UDP socket: dual-stack when the system allows (a v6 player), IPv4 otherwise.
        int fd = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
        if (fd >= 0) {
            int off = 0;
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
            sockaddr_in6 any{};
            any.sin6_family = AF_INET6;
            any.sin6_addr = in6addr_any;
            if (::bind(fd, reinterpret_cast<sockaddr*>(&any), sizeof any) != 0) ::close(fd), fd = -1;
        }
        if (fd < 0) {
            fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            sockaddr_in any{};
            any.sin_family = AF_INET;
            if (fd >= 0 && ::bind(fd, reinterpret_cast<sockaddr*>(&any), sizeof any) != 0) ::close(fd), fd = -1;
        }
        if (fd < 0) return false;
        nonblocking(fd);
        c.udp = fd;
        c.open = true;
        c.last = c.token_time = now;
        c.tokens = opt.bytes_per_s;
        size_t n = 0;
        {
            std::lock_guard<std::mutex> lock(stats_mu);
            ++stats.accepted;
            for (const Client& o : clients) n += o.open ? 1 : 0;
            stats.open = n;
        }
        say("browser connected (" + std::to_string(n) + " open" + (c.who.empty() ? "" : ", " + c.who) + ")");
        return true;
    }

    void send_udp(Client& c, const Address& to, const u8* data, size_t n) {
        Address dst = to;
        sockaddr_in6 mapped{};
        sockaddr_storage me{};
        socklen_t ml = sizeof me;
        if (to.family() == AF_INET && getsockname(c.udp, reinterpret_cast<sockaddr*>(&me), &ml) == 0 &&
            me.ss_family == AF_INET6) {
            const auto* v4 = reinterpret_cast<const sockaddr_in*>(to.storage);
            mapped.sin6_family = AF_INET6;
            mapped.sin6_port = v4->sin_port;
            u8* b = reinterpret_cast<u8*>(&mapped.sin6_addr);
            b[10] = b[11] = 0xFF;
            std::memcpy(b + 12, &v4->sin_addr, 4);
            std::memcpy(dst.storage, &mapped, sizeof mapped);
            dst.length = sizeof mapped;
        }
        ::sendto(c.udp, data, n, 0, reinterpret_cast<const sockaddr*>(dst.storage), socklen_t(dst.length));
    }

    // One message from the browser: a datagram to send on.
    void from_browser(Client& c, const u8* p, size_t n, double now) {
        if (n < 2 || p[0] != 0) return;
        Address to;
        size_t used = 0;
        if (!get_address(p + 1, n - 1, opt.relay, to, used)) return;
        const u8* data = p + 1 + used;
        const size_t len = n - 1 - used;
        bool ok = to == opt.relay || (opt.allow_lan && private_v4(to));
        for (const Address& a : c.heard) ok = ok || a == to;
        c.tokens = std::min(opt.bytes_per_s, c.tokens + (now - c.token_time) / 1000 * opt.bytes_per_s);
        c.token_time = now;
        if (!ok || double(len) > c.tokens) {
            count(&Stats::refused);
            return;
        }
        c.tokens -= double(len);
        send_udp(c, to, data, len);
        count(&Stats::to_udp);
    }

    // The frames that arrived (RFC 6455 section 5): binary messages, pings, the close.
    bool frames(Client& c, double now) {
        while (c.in.size() >= 2) {
            const u8* p = reinterpret_cast<const u8*>(c.in.data());
            const bool fin = p[0] & 0x80, masked = p[1] & 0x80;
            const int opcode = p[0] & 0x0F;
            u64 len = p[1] & 0x7F;
            size_t pos = 2;
            if (len == 126) {
                if (c.in.size() < 4) return true;
                len = u64(p[2]) << 8 | p[3];
                pos = 4;
            } else if (len == 127) {
                if (c.in.size() < 10) return true;
                len = 0;
                for (int i = 0; i < 8; ++i) len = len << 8 | p[2 + i];
                pos = 10;
            }
            if (!masked || len > kMaxMessage || !fin || opcode == 0) return false; // (browsers mask; no fragments here)
            if (c.in.size() < pos + 4 + len) return true;
            u8 mask[4];
            std::memcpy(mask, p + pos, 4);
            std::string payload = c.in.substr(pos + 4, size_t(len));
            for (size_t i = 0; i < payload.size(); ++i) payload[i] = char(payload[i] ^ mask[i & 3]);
            c.in.erase(0, pos + 4 + size_t(len));
            c.last = now;
            if (opcode == 2) from_browser(c, reinterpret_cast<const u8*>(payload.data()), payload.size(), now);
            else if (opcode == 9) frame(c.out, 10, payload.data(), payload.size()); // ping -> pong
            else if (opcode == 8) {
                frame(c.out, 8, payload.data(), std::min<size_t>(payload.size(), 2));
                c.closing = true;
                return true;
            } else if (opcode != 1 && opcode != 10) {
                return false;
            }
        }
        return true;
    }

    // Datagrams for the browser.
    void from_udp(Client& c) {
        for (int i = 0; i < 64; ++i) {
            u8 buf[kMaxMessage];
            Address from;
            socklen_t len = sizeof from.storage;
            const ssize_t n = ::recvfrom(c.udp, buf, sizeof buf - 20, 0, reinterpret_cast<sockaddr*>(from.storage), &len);
            if (n < 0) return;
            from.length = u32(len);
            from = normalized(from);
            const bool relay = from == opt.relay;
            if (!relay) {
                bool known = false;
                for (const Address& a : c.heard) known = known || a == from;
                if (!known) {
                    if (c.heard.size() >= kMaxAllowed) c.heard.erase(c.heard.begin());
                    c.heard.push_back(from);
                }
            }
            if (c.out.size() > kMaxBacklog) {
                count(&Stats::dropped);
                continue;
            }
            std::string m(1, char(0));
            put_address(m, from, relay);
            m.append(reinterpret_cast<const char*>(buf), size_t(n));
            frame(c.out, 2, m.data(), m.size());
            count(&Stats::to_browser);
        }
    }

    void run() {
        std::vector<pollfd> fds;
        std::vector<std::pair<size_t, int>> owner; // (client, 0 tcp / 1 udp) of each fds entry after the first
        while (!stopping) {
            fds.assign(1, pollfd{listen_fd, POLLIN, 0});
            owner.clear();
            for (size_t i = 0; i < clients.size(); ++i) {
                const Client& c = clients[i];
                fds.push_back({c.tcp, short(POLLIN | (c.out.empty() ? 0 : POLLOUT)), 0});
                owner.push_back({i, 0});
                if (c.udp >= 0) {
                    fds.push_back({c.udp, POLLIN, 0});
                    owner.push_back({i, 1});
                }
            }
            if (::poll(fds.data(), nfds_t(fds.size()), 50) < 0 && errno != EINTR) break;
            const double now = now_ms();
            if (fds[0].revents & POLLIN) accept_clients(now);
            for (size_t k = 1; k < fds.size(); ++k) {
                Client& c = clients[owner[k - 1].first];
                if (c.tcp < 0 || !fds[k].revents) continue;
                if (owner[k - 1].second == 1) {
                    from_udp(c);
                    continue;
                }
                if (fds[k].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    close_client(c, "connection lost");
                    continue;
                }
                if (fds[k].revents & POLLIN) {
                    char buf[16384];
                    const ssize_t n = ::recv(c.tcp, buf, sizeof buf, 0);
                    if (n <= 0) {
                        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) close_client(c, "closed");
                        continue;
                    }
                    c.in.append(buf, size_t(n));
                    const bool ok = c.open ? frames(c, now) : handshake(c, now);
                    if (!ok) {
                        close_client(c, "bad request");
                        continue;
                    }
                }
            }
            for (Client& c : clients) {
                if (c.tcp < 0) continue;
                if (!c.out.empty()) {
                    const ssize_t n = ::send(c.tcp, c.out.data(), c.out.size(), MSG_NOSIGNAL);
                    if (n > 0) c.out.erase(0, size_t(n));
                    else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) close_client(c, "connection lost");
                }
                if (c.tcp >= 0 && c.closing && c.out.empty()) close_client(c, "closed");
                else if (c.tcp >= 0 && (c.open ? now - c.last > kIdleMs : now - c.since > kHandshakeMs))
                    close_client(c, c.open ? "idle" : "no handshake");
            }
            size_t before = clients.size();
            for (size_t i = clients.size(); i-- > 0;)
                if (clients[i].tcp < 0) clients.erase(clients.begin() + long(i));
            if (clients.size() != before) {
                std::lock_guard<std::mutex> lock(stats_mu);
                size_t n = 0;
                for (const Client& c : clients) n += c.open ? 1 : 0;
                stats.open = n;
            }
        }
        for (Client& c : clients) close_client(c, "stopping");
        clients.clear();
    }

    void accept_clients(double now) {
        for (;;) {
            sockaddr_storage from{};
            socklen_t len = sizeof from;
            const int fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&from), &len);
            if (fd < 0) return;
            if (clients.size() >= opt.max_clients) {
                ::close(fd);
                count(&Stats::refused);
                continue;
            }
            nonblocking(fd);
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
            Client c;
            c.tcp = fd;
            c.since = c.last = now;
            if (opt.log_ips) {
                Address a;
                std::memcpy(a.storage, &from, std::min<size_t>(len, sizeof a.storage));
                a.length = u32(len);
                c.who = normalized(a).to_string();
            }
            clients.push_back(std::move(c));
        }
    }
};

WsGateway::WsGateway() : impl_(std::make_unique<Impl>()) {}
WsGateway::~WsGateway() { stop(); }

bool WsGateway::start(const Options& options, std::function<void(const std::string&)> log, std::string* error) {
    Impl& m = *impl_;
    m.opt = options;
    m.opt.relay = normalized(options.relay);
    m.log = std::move(log);
    int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    bool v6 = fd >= 0;
    if (!v6) fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (error) *error = "socket() failed";
        return false;
    }
    int one = 1, off = 0;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    int rc;
    if (v6) {
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_addr = in6addr_any;
        a.sin6_port = htons(options.port);
        rc = ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    } else {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(options.port);
        rc = ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    }
    if (rc != 0 || ::listen(fd, 64) != 0) {
        ::close(fd);
        if (error) *error = "cannot use TCP port " + std::to_string(options.port) + " (in use?)";
        return false;
    }
    nonblocking(fd);
    m.listen_fd = fd;
    m.stopping = false;
    m.thread = std::thread([&m] { m.run(); });
    return true;
}

void WsGateway::stop() {
    if (!impl_ || !impl_->thread.joinable()) return;
    impl_->stopping = true;
    impl_->thread.join();
    ::close(impl_->listen_fd);
    impl_->listen_fd = -1;
}

WsGateway::Stats WsGateway::stats() const {
    std::lock_guard<std::mutex> lock(impl_->stats_mu);
    return impl_->stats;
}

#endif

} // namespace gt3r::net
