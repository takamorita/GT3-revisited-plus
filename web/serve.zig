// A small web server for playing the browser build on this computer: it serves the folder it is in (or the one
// given) on http://localhost:8080/ with the Cross-Origin-Opener-Policy and Cross-Origin-Embedder-Policy headers
// the engine's threads need (SharedArrayBuffer), and opens the browser there. Only this computer can connect.
//
//   serve [folder] [--port N] [--no-open]
//
// The port is the first free one from 8080 (or N). Build (Zig 0.15), e.g. for Windows:
//   zig build-exe serve.zig -O ReleaseSmall -fstrip -target x86_64-windows
const std = @import("std");

const default_port: u16 = 8080;
const max_head = 16 * 1024;

var root: std.fs.Dir = undefined;

pub fn main() !void {
    const alloc = std.heap.page_allocator;
    const args = try std.process.argsAlloc(alloc);
    defer std.process.argsFree(alloc, args);

    var folder: ?[]const u8 = null;
    var port: u16 = default_port;
    var open_browser = true;
    var i: usize = 1;
    while (i < args.len) : (i += 1) {
        const a = args[i];
        if (std.mem.eql(u8, a, "--port") and i + 1 < args.len) {
            i += 1;
            port = std.fmt.parseInt(u16, args[i], 10) catch return usage();
        } else if (std.mem.eql(u8, a, "--no-open")) {
            open_browser = false;
        } else if (std.mem.eql(u8, a, "-h") or std.mem.eql(u8, a, "--help")) {
            return usage();
        } else if (folder == null) {
            folder = a;
        } else return usage();
    }

    // The folder: the one given, else the executable's own (double-clicked next to index.html).
    const dir_path = if (folder) |f| try alloc.dupe(u8, f) else try std.fs.selfExeDirPathAlloc(alloc);
    root = std.fs.cwd().openDir(dir_path, .{}) catch |e| {
        std.debug.print("Cannot open the folder {s}: {s}\n", .{ dir_path, @errorName(e) });
        return e;
    };
    if (root.access("index.html", .{})) |_| {} else |_| {
        std.debug.print("Note: there is no index.html in {s}\n", .{dir_path});
    }

    var server: std.net.Server = undefined;
    var tries: u16 = 0;
    while (true) : (tries += 1) {
        const address = try std.net.Address.parseIp("127.0.0.1", port);
        if (address.listen(.{ .reuse_address = true })) |s| {
            server = s;
            break;
        } else |e| {
            if (e != error.AddressInUse or tries >= 20 or port == std.math.maxInt(u16)) {
                std.debug.print("Cannot listen on port {d}: {s}\n", .{ port, @errorName(e) });
                return e;
            }
            port += 1;
        }
    }

    var url_buf: [64]u8 = undefined;
    const url = try std.fmt.bufPrint(&url_buf, "http://localhost:{d}/", .{port});
    std.debug.print("GT3 Revisited: {s}  (serving {s}; close this window or press Ctrl+C to stop)\n", .{ url, dir_path });
    if (open_browser) openBrowser(alloc, url);

    while (true) {
        const conn = server.accept() catch continue;
        const t = std.Thread.spawn(.{}, handle, .{conn.stream}) catch {
            conn.stream.close();
            continue;
        };
        t.detach();
    }
}

fn usage() void {
    std.debug.print("usage: serve [folder] [--port N] [--no-open]\n", .{});
}

fn openBrowser(alloc: std.mem.Allocator, url: []const u8) void {
    const argv: []const []const u8 = switch (@import("builtin").os.tag) {
        .windows => &.{ "rundll32", "url.dll,FileProtocolHandler", url },
        .macos => &.{ "open", url },
        else => &.{ "xdg-open", url },
    };
    var child = std.process.Child.init(argv, alloc);
    child.stdin_behavior = .Ignore;
    child.stdout_behavior = .Ignore;
    child.stderr_behavior = .Ignore;
    _ = child.spawnAndWait() catch {};
}

// One request per connection (Connection: close).
fn handle(stream: std.net.Stream) void {
    defer stream.close();
    var head: [max_head]u8 = undefined;
    var n: usize = 0;
    while (std.mem.indexOf(u8, head[0..n], "\r\n\r\n") == null) {
        if (n == head.len) return send(stream, 431, "Request Header Fields Too Large");
        const got = recvSome(stream, head[n..]) catch return;
        if (got == 0) return;
        n += got;
    }
    const line_end = std.mem.indexOf(u8, head[0..n], "\r\n").?;
    var parts = std.mem.splitScalar(u8, head[0..line_end], ' ');
    const method = parts.next() orelse return;
    const target = parts.next() orelse return;
    const is_head = std.mem.eql(u8, method, "HEAD");
    if (!is_head and !std.mem.eql(u8, method, "GET")) return send(stream, 405, "Method Not Allowed");

    var path_buf: [1024]u8 = undefined;
    const path = decodePath(target, &path_buf) orelse return send(stream, 400, "Bad Request");
    serveFile(stream, path, is_head) catch {};
}

// The request target as a path in the folder: no query, percent-decoded, "/" -> index.html, no "..".
fn decodePath(target: []const u8, out: []u8) ?[]const u8 {
    const raw = target[0 .. std.mem.indexOfAny(u8, target, "?#") orelse target.len];
    if (raw.len == 0 or raw[0] != '/') return null;
    var len: usize = 0;
    var i: usize = 1;
    while (i < raw.len) : (i += 1) {
        var c = raw[i];
        if (c == '%') {
            if (i + 2 >= raw.len) return null;
            c = std.fmt.parseInt(u8, raw[i + 1 .. i + 3], 16) catch return null;
            i += 2;
        }
        if (c == 0 or c == '\\' or len == out.len) return null;
        out[len] = c;
        len += 1;
    }
    var p = out[0..len];
    if (p.len == 0 or p[p.len - 1] == '/') {
        const index = "index.html";
        if (len + index.len > out.len) return null;
        @memcpy(out[len .. len + index.len], index);
        p = out[0 .. len + index.len];
    }
    var it = std.mem.splitScalar(u8, p, '/');
    while (it.next()) |seg| {
        if (seg.len == 0 or std.mem.eql(u8, seg, ".") or std.mem.eql(u8, seg, "..")) return null;
    }
    return p;
}

fn serveFile(stream: std.net.Stream, path: []const u8, is_head: bool) !void {
    const file = root.openFile(path, .{}) catch return send(stream, 404, "Not Found");
    defer file.close();
    const st = try file.stat();
    if (st.kind != .file) return send(stream, 404, "Not Found");

    var hdr: [512]u8 = undefined;
    const h = try std.fmt.bufPrint(&hdr, "HTTP/1.1 200 OK\r\n" ++
        "Content-Type: {s}\r\nContent-Length: {d}\r\n" ++
        "Cross-Origin-Opener-Policy: same-origin\r\nCross-Origin-Embedder-Policy: require-corp\r\n" ++
        "Cross-Origin-Resource-Policy: same-origin\r\n" ++
        // (the debug overlay's profiler, debug.js)
        "Document-Policy: js-profiling\r\n" ++
        "Cache-Control: no-cache\r\nConnection: close\r\n\r\n", .{ mimeType(path), st.size });
    try sendAll(stream, h);
    if (is_head) return;
    var buf: [64 * 1024]u8 = undefined;
    while (true) {
        const got = try file.read(&buf);
        if (got == 0) break;
        try sendAll(stream, buf[0..got]);
    }
}

// Socket reads and writes through recv() and send(): Winsock's on Windows (Stream.read failed on its sockets
// there), POSIX's elsewhere (a client that leaves early: an error, not SIGPIPE, which Zig's start code ignores).
const is_windows = @import("builtin").os.tag == .windows;

fn recvSome(stream: std.net.Stream, buf: []u8) !usize {
    if (is_windows) {
        const ws2 = std.os.windows.ws2_32;
        const r = ws2.recv(stream.handle, buf.ptr, @intCast(@min(buf.len, 1 << 30)), 0);
        if (r == ws2.SOCKET_ERROR) return error.ReceiveFailed;
        return @intCast(r);
    }
    return std.posix.recv(stream.handle, buf, 0);
}

fn sendAll(stream: std.net.Stream, data: []const u8) !void {
    var off: usize = 0;
    while (off < data.len) {
        if (is_windows) {
            const ws2 = std.os.windows.ws2_32;
            const r = ws2.send(stream.handle, data[off..].ptr, @intCast(@min(data.len - off, 1 << 30)), 0);
            if (r == ws2.SOCKET_ERROR) return error.SendFailed;
            off += @intCast(r);
        } else {
            off += try std.posix.send(stream.handle, data[off..], 0);
        }
    }
}

fn send(stream: std.net.Stream, code: u16, text: []const u8) void {
    var hdr: [256]u8 = undefined;
    const h = std.fmt.bufPrint(&hdr, "HTTP/1.1 {d} {s}\r\nContent-Type: text/plain\r\nContent-Length: {d}\r\n" ++
        "Connection: close\r\n\r\n{s}", .{ code, text, text.len, text }) catch return;
    sendAll(stream, h) catch {};
}

fn mimeType(path: []const u8) []const u8 {
    const ext = std.fs.path.extension(path);
    const types = [_][2][]const u8{
        .{ ".html", "text/html; charset=utf-8" },
        .{ ".js", "text/javascript; charset=utf-8" },
        .{ ".mjs", "text/javascript; charset=utf-8" },
        .{ ".wasm", "application/wasm" },
        .{ ".json", "application/json" },
        .{ ".css", "text/css; charset=utf-8" },
        .{ ".txt", "text/plain; charset=utf-8" },
        .{ ".md", "text/plain; charset=utf-8" },
        .{ ".png", "image/png" },
        .{ ".svg", "image/svg+xml" },
        .{ ".ico", "image/x-icon" },
    };
    for (types) |t| {
        if (std.ascii.eqlIgnoreCase(ext, t[0])) return t[1];
    }
    return "application/octet-stream";
}
