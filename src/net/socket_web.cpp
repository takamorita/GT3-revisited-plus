// The browser's UdpSocket (socket.h): a browser has no UDP, so each socket is a WebSocket to a relay
// server's port for browsers (gt3r-relay --ws-port, tools/gt3r-relay/ws_gateway.h), which sends the
// datagrams on from a UDP socket of its own and passes back what arrives. The relay server's address
// in Settings / the online page is then its WebSocket: ws://host:port or wss://host/path.
//
// The WebSocket lives in a worker of its own (web/net-worker.js), so the game's threads never wait on
// the page: the two share a Link in the game's memory, two rings of messages (the gateway's: a kind
// byte, the other end's address, the datagram) with counters for Atomics. The receive thread sleeps
// on the "in" counter (a futex); the worker waits on the "out" one (Atomics.waitAsync).
#ifdef __EMSCRIPTEN__

#include "net/socket.h"

#include <emscripten.h>
#include <emscripten/threading.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace gt3r::net {

namespace {

constexpr int kSlots = 128;       // datagrams waiting each way (a race sends 60 a second)
constexpr int kSlotBytes = 1600;  // a datagram and its address
// ctrl[]: written by both sides with Atomics (net-worker.js has the same numbers).
enum Ctrl { kState, kInWrite, kInRead, kOutWrite, kOutRead, kCtrlCount = 8 };
enum State { kConnecting, kOpen, kClosed, kClosing, kDone }; // kDone: the worker has gone, the Link is free

struct Slot {
    int32_t len;
    u8 data[kSlotBytes];
};
static_assert(sizeof(Slot) == 4 + kSlotBytes, "net-worker.js counts a slot as 4 + slotBytes bytes");
struct Link {
    alignas(8) int32_t ctrl[kCtrlCount];
    Slot in[kSlots];
    Slot out[kSlots];
    std::mutex send_mu; // several threads send
};

std::mutex g_mu;
std::vector<Link*> g_links; // a socket's fd is its index here
std::string g_url;          // the relay's WebSocket, from the last resolve()

int32_t load(const int32_t& v) { return __atomic_load_n(&v, __ATOMIC_SEQ_CST); }
void store(int32_t& v, int32_t x) { __atomic_store_n(&v, x, __ATOMIC_SEQ_CST); }

// The relay server stands for itself in the gateway's messages; its Address here is this one.
Address relay_address() {
    Address a;
    auto* s = reinterpret_cast<sockaddr_in*>(a.storage);
    s->sin_family = AF_INET; // 0.0.0.0:0
    a.length = sizeof(sockaddr_in);
    return a;
}

bool is_relay(const Address& a) { return a.valid() && a == relay_address(); }

Link* link_of(int fd) {
    std::lock_guard<std::mutex> lock(g_mu);
    return fd >= 0 && size_t(fd) < g_links.size() ? g_links[size_t(fd)] : nullptr;
}

bool open_link(int& fd, std::string* error) {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        url = g_url;
    }
    if (url.empty()) {
        if (error) *error = "in a browser online play goes through a relay server's WebSocket: set the relay server to its ws:// or wss:// address";
        return false;
    }
    Link* l = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (size_t i = 0; i < g_links.size() && !l; ++i)
            if (load(g_links[i]->ctrl[kState]) == kDone) l = g_links[i], fd = int(i);
        if (!l) {
            l = new Link;
            g_links.push_back(l);
            fd = int(g_links.size() - 1);
        }
        for (int32_t& c : l->ctrl) store(c, 0); // kConnecting, empty rings
    }
    // The worker starts on the page's thread (it outlives whichever thread opened the socket).
    MAIN_THREAD_EM_ASM(
        {
            const url = new TextDecoder().decode(HEAPU8.slice($0, $0 + $1));
            const worker = new Worker('net-worker.js');
            worker.postMessage({memory : HEAPU8.buffer, base : $2, slots : $3, slotBytes : $4, url : url});
        },
        url.data(), int(url.size()), l, kSlots, kSlotBytes);
    return true;
}

// The gateway's address bytes for `to` (the family byte 0 = the relay).
void put_address(std::vector<u8>& m, const Address& to) {
    if (is_relay(to)) {
        m.push_back(0);
        return;
    }
    u8 ip16[16];
    to.ip16(ip16);
    static const u8 kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    const bool v4 = std::memcmp(ip16, kMapped, 12) == 0;
    m.push_back(v4 ? 4 : 6);
    m.insert(m.end(), v4 ? ip16 + 12 : ip16, ip16 + 16);
    const u16 port = to.port();
    m.push_back(u8(port >> 8));
    m.push_back(u8(port & 0xFF));
}

} // namespace

bool resolve_web_relay(const std::string& text, Address& out) {
    if (text.rfind("ws://", 0) != 0 && text.rfind("wss://", 0) != 0) return false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_url = text;
    }
    out = relay_address();
    return true;
}

bool UdpSocket::open(u16, std::string* error) {
    close();
    int fd = -1;
    if (!open_link(fd, error)) return false;
    fd_ = fd;
    return true;
}

bool UdpSocket::open_for(const Address&, std::string* error) { return open(0, error); }

void UdpSocket::close() {
    if (fd_ == kInvalid) return;
    if (Link* l = link_of(fd_)) {
        // The worker closes the WebSocket and marks the Link done (free for the next socket).
        __atomic_store_n(&l->ctrl[kState], kClosing, __ATOMIC_SEQ_CST);
        emscripten_futex_wake(&l->ctrl[kOutWrite], 1);
    }
    fd_ = kInvalid;
}

int UdpSocket::set_buffers(int) { return 0; }

u16 UdpSocket::local_port() const { return 0; }

bool UdpSocket::send_to(const Address& to, ByteSpan data) {
    Link* l = link_of(fd_);
    if (!l || !to.valid() || load(l->ctrl[kState]) >= kClosed) return false;
    std::vector<u8> m;
    m.reserve(data.size() + 20);
    m.push_back(0); // a datagram
    put_address(m, to);
    m.insert(m.end(), data.begin(), data.end());
    if (m.size() > size_t(kSlotBytes)) return false;
    std::lock_guard<std::mutex> lock(l->send_mu);
    const int32_t w = load(l->ctrl[kOutWrite]);
    if (w - load(l->ctrl[kOutRead]) >= kSlots) return false; // full: dropped, as UDP would
    Slot& s = l->out[w % kSlots];
    std::memcpy(s.data, m.data(), m.size());
    s.len = int32_t(m.size());
    store(l->ctrl[kOutWrite], w + 1);
    emscripten_futex_wake(&l->ctrl[kOutWrite], 1); // (Atomics.notify: the worker's waitAsync)
    return true;
}

size_t UdpSocket::recv_from(Address& from, u8* buf, size_t cap) {
    Link* l = link_of(fd_);
    if (!l) return 0;
    for (;;) {
        const int32_t r = load(l->ctrl[kInRead]);
        if (r == load(l->ctrl[kInWrite])) return 0;
        const Slot& s = l->in[r % kSlots];
        const u8* p = s.data;
        const size_t n = size_t(s.len);
        store(l->ctrl[kInRead], r + 1);
        // kind 0, then the address: 0 (the relay) | 4 + 4 bytes + port | 6 + 16 bytes + port
        if (n < 2 || p[0] != 0) continue;
        size_t at = 2;
        Address a;
        if (p[1] == 0) {
            a = relay_address();
        } else if ((p[1] == 4 && n >= 8) || (p[1] == 6 && n >= 20)) {
            const size_t ip = p[1] == 4 ? 4 : 16;
            const u16 port = u16(p[2 + ip] << 8 | p[3 + ip]);
            if (ip == 4) {
                auto* sa = reinterpret_cast<sockaddr_in*>(a.storage);
                sa->sin_family = AF_INET;
                sa->sin_port = htons(port);
                std::memcpy(&sa->sin_addr, p + 2, 4);
                a.length = sizeof(sockaddr_in);
            } else {
                auto* sa = reinterpret_cast<sockaddr_in6*>(a.storage);
                sa->sin6_family = AF_INET6;
                sa->sin6_port = htons(port);
                std::memcpy(&sa->sin6_addr, p + 2, 16);
                a.length = sizeof(sockaddr_in6);
            }
            at = 2 + ip + 2;
        } else {
            continue;
        }
        const size_t len = n - at;
        if (len > cap) continue;
        std::memcpy(buf, p + at, len);
        from = a;
        return len;
    }
}

bool UdpSocket::wait_readable(int timeout_ms) const {
    Link* l = link_of(fd_);
    if (!l) return false;
    const int32_t w = load(l->ctrl[kInWrite]);
    if (load(l->ctrl[kInRead]) != w) return true;
    if (load(l->ctrl[kState]) >= kClosed) {
        emscripten_thread_sleep(timeout_ms); // (as a socket with nothing to read would wait)
        return false;
    }
    emscripten_futex_wait(&l->ctrl[kInWrite], uint32_t(w), double(timeout_ms));
    return load(l->ctrl[kInRead]) != load(l->ctrl[kInWrite]);
}

} // namespace gt3r::net

#endif // __EMSCRIPTEN__
