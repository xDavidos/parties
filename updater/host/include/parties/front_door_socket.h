// Where the launcher's front-door socket lives, and the options both ends set
// on every front-door descriptor. Shared by the host and the launcher.

#pragma once

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <string_view>

namespace parties::update::front_door {

// $(getconf DARWIN_USER_TEMP_DIR)org.parties.client[-<namespace>]. Empty when
// the per-user temporary directory is unavailable.
inline std::string socket_dir(std::string_view test_namespace) {
    char temp[1024];
    size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, temp, sizeof temp);
    if (n == 0 || n > sizeof temp) return {};
    std::string dir = std::string(temp) + "org.parties.client";
    if (!test_namespace.empty()) dir.append("-").append(test_namespace);
    return dir;
}

inline std::string socket_path(std::string_view test_namespace) {
    std::string dir = socket_dir(test_namespace);
    return dir.empty() ? dir : dir + "/launcher.sock";
}

inline bool socket_address(const std::string& path, sockaddr_un& address) {
    if (path.empty() || path.size() >= sizeof address.sun_path) return false;
    address = {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return true;
}

// Close-on-exec, because a child that inherited the descriptor (Sentry's crash
// handler) would keep the connection open after its front died, and the
// launcher would never see EOF. No SIGPIPE, because writing to a peer that
// has died is expected and must not kill the writer.
inline void prepare_socket(int fd) {
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
}

} // namespace parties::update::front_door
