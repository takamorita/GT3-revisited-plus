#pragma once

// The relay's port for browsers (gt3r-relay --ws-port): a browser has no UDP, so the browser build
// (src/net/socket_web.cpp, web/net-worker.js) carries its datagrams over a WebSocket to here, and each
// connection gets a UDP socket of its own on this machine that sends them on and passes back what
// arrives. To the relay and to the other players a browser is then an ordinary UDP peer at this
// machine's address: nothing in the game's protocols changes, and native players need nothing new.
//
// A WebSocket message is one datagram: a kind byte (0), the other end's address (a family byte:
// 0 = the relay server, 4 = IPv4 then 4 address bytes, 6 = IPv6 then 16; then the port, big-endian,
// except for the relay) and the datagram's bytes.
//
// A connection's UDP socket sends only to the relay, to addresses that have sent to it, and (with
// allow_lan) to private IPv4 addresses (10/8, 172.16/12, 192.168/16): it is no open UDP proxy.
// Plain ws:// only; wss:// (needed by a page served over https) is a TLS proxy in front (Caddy,
// nginx, ...) that passes the connection on. POSIX only (Linux, macOS), like the relay's deployments.

#include "net/socket.h"

#include <functional>
#include <memory>
#include <string>

namespace gt3r::net {

class WsGateway {
public:
    struct Options {
        u16 port = 0;            // TCP port for the WebSockets
        Address relay;           // the relay's UDP address as the other players reach it (not loopback)
        bool allow_lan = false;  // also send to private IPv4 addresses (LAN play)
        size_t max_clients = 200;
        double bytes_per_s = 64 * 1024; // per connection, browser -> UDP (a race uses about 10 KB/s)
        bool log_ips = false;
    };
    WsGateway();
    ~WsGateway();
    // Listens and starts its thread. False: `error` says why (port in use, no POSIX sockets).
    bool start(const Options& options, std::function<void(const std::string&)> log, std::string* error);
    void stop();
    struct Stats {
        size_t open = 0;            // connections now
        unsigned long long accepted = 0, to_udp = 0, to_browser = 0, refused = 0, dropped = 0;
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gt3r::net
