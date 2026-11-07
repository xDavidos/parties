#include "front_door_client.h"

#import <AppKit/AppKit.h>

#include <parties/front_door_socket.h>
#include <parties/update/link.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

namespace parties::host::front_door {

namespace {

int g_fd = -1;
std::mutex g_write;
// Read by introduce() until the payload starts, then only by the watcher.
update::link::LineReader g_reader;

int connect_socket(const std::string& path) {
    sockaddr_un address;
    if (!protocol::socket_address(path, address)) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    protocol::prepare_socket(fd);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void write_all(int fd, const std::string& line) {
    for (size_t sent = 0; sent < line.size();) {
        ssize_t n = ::send(fd, line.data() + sent, line.size() - sent, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        sent += size_t(n);
    }
}

std::optional<std::string> read_line() {
    for (;;) {
        if (auto line = g_reader.next()) return line;
        char buffer[4096];
        ssize_t n = recv(g_fd, buffer, sizeof buffer, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return std::nullopt;
        g_reader.append({buffer, size_t(n)});
    }
}

} // namespace

bool connect(const std::string& socket_path) {
    if (g_fd >= 0) close(g_fd);
    g_reader = {};
    g_fd = connect_socket(socket_path);
    return g_fd >= 0;
}

std::optional<protocol::LauncherMessage> introduce(const protocol::Front& front) {
    send(front);
    while (auto line = read_line()) {
        auto message = protocol::parse_launcher_message(*line);
        if (message && !std::holds_alternative<protocol::Activate>(*message)) return message;
    }
    return std::nullopt;
}

int attach_link(const std::string& socket_path, uint64_t ticket) {
    int fd = connect_socket(socket_path);
    if (fd >= 0) write_all(fd, protocol::write(protocol::HostMessage{protocol::Attach{ticket}}));
    return fd;
}

void send(const protocol::HostMessage& message) {
    std::lock_guard lock(g_write);
    if (g_fd >= 0) write_all(g_fd, protocol::write(message));
}

void exit_process(int code) {
    g_write.lock();
    if (g_fd >= 0) write_all(g_fd, protocol::write(protocol::HostMessage{protocol::Exiting{code}}));
    std::fprintf(stderr, "[host] exit_process(%d)\n", code);
    _exit(code);
}

void watch_launcher() {
    std::thread([] {
        while (auto line = read_line()) {
            auto message = protocol::parse_launcher_message(*line);
            if (!message || !std::holds_alternative<protocol::Activate>(*message)) continue;
            std::fprintf(stderr, "[host] activate\n");
            dispatch_async(dispatch_get_main_queue(), ^{
                [NSRunningApplication.currentApplication activateWithOptions:NSApplicationActivateAllWindows];
            });
        }
        std::fprintf(stderr, "[host] launcher lost: asking the app to quit\n");
        dispatch_async(dispatch_get_main_queue(), ^{
            [NSApp terminate:nil];
        });
        std::this_thread::sleep_for(std::chrono::seconds(15));
        exit_process(1);
    }).detach();
}

} // namespace parties::host::front_door
