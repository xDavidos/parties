#include "front_door.h"

#import <AppKit/AppKit.h>

#include <parties/front_door_socket.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace parties::update::launcher::macos {

namespace {

template <class... F>
struct overloaded : F... {
    using F::operator()...;
};

__attribute__((format(printf, 2, 3))) void trace(double ms, const char* format, ...) {
    std::fprintf(stderr, "[front-door] %7.0f ms ", ms);
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}

void add_filter(int kq, uintptr_t ident, int16_t filter, uint16_t flags, uint32_t fflags, intptr_t data) {
    struct kevent change;
    EV_SET(&change, ident, filter, flags, fflags, data, nullptr);
    kevent(kq, &change, 1, nullptr, 0, nullptr);
}

} // namespace

const char* exit_kind_name(ExitKind kind) {
    switch (kind) {
    case ExitKind::Clean: return "Clean";
    case ExitKind::InitFailed: return "InitFailed";
    case ExitKind::DataUnavailable: return "DataUnavailable";
    case ExitKind::DataTooNew: return "DataTooNew";
    case ExitKind::Abnormal: return "Abnormal";
    case ExitKind::Terminated: return "Terminated";
    case ExitKind::KilledHung: return "KilledHung";
    }
    return "?";
}

void launch_front(const std::string& bundle, const std::vector<std::string>& arguments, bool new_instance) {
    NSWorkspaceOpenConfiguration* configuration = [NSWorkspaceOpenConfiguration configuration];
    NSMutableArray<NSString*>* args = [NSMutableArray arrayWithCapacity:arguments.size()];
    for (const std::string& arg : arguments) [args addObject:@(arg.c_str())];
    configuration.arguments = args;
    configuration.activates = YES;
    configuration.createsNewApplicationInstance = new_instance;
    NSURL* url = [NSURL fileURLWithPath:@(bundle.c_str()) isDirectory:YES];
    const auto started = std::chrono::steady_clock::now();
    std::fprintf(stderr, "[front-door] NSWorkspace open createsNewApplicationInstance=%s %s\n",
                 new_instance ? "YES" : "NO", bundle.c_str());
    [NSWorkspace.sharedWorkspace openApplicationAtURL:url
                                        configuration:configuration
                                    completionHandler:^(NSRunningApplication* app, NSError* error) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (app)
            std::fprintf(stderr, "[front-door] NSWorkspace answered pid=%d after %.0f ms (createsNewApplicationInstance=%s)\n",
                         app.processIdentifier, ms, new_instance ? "YES" : "NO");
        else
            std::fprintf(stderr, "[front-door] NSWorkspace failed after %.0f ms: %s\n", ms,
                         error.description.UTF8String);
    }];
}

std::unique_ptr<FrontDoor> FrontDoor::listen(Options options) {
    const std::string& path = options.socket_path;
    sockaddr_un address;
    if (!front_door::socket_address(path, address)) {
        std::fprintf(stderr, "[front-door] unusable socket path '%s'\n", path.c_str());
        return nullptr;
    }
    mkdir(path.substr(0, path.rfind('/')).c_str(), 0700);
    unlink(path.c_str());
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) return nullptr;
    front_door::prepare_socket(listener);
    fcntl(listener, F_SETFL, O_NONBLOCK);
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0 ||
        ::listen(listener, 16) != 0) {
        std::fprintf(stderr, "[front-door] cannot listen on %s: %s\n", path.c_str(), std::strerror(errno));
        close(listener);
        return nullptr;
    }
    int kq = kqueue();
    add_filter(kq, uintptr_t(listener), EVFILT_READ, EV_ADD, 0, 0);
    std::fprintf(stderr, "[front-door] listening on %s\n", path.c_str());
    return std::unique_ptr<FrontDoor>(new FrontDoor(std::move(options), kq, listener));
}

FrontDoor::FrontDoor(Options options, int kq, int listener)
    : options_(std::move(options)), kq_(kq), listener_(listener) {}

FrontDoor::~FrontDoor() {
    for (const auto& [fd, conn] : conns_) close(fd);
    close(listener_);
    close(kq_);
    unlink(options_.socket_path.c_str());
}

double FrontDoor::now_ms() const {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started_).count();
}

std::optional<Event> FrontDoor::next(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (events_.empty()) {
        const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return std::nullopt;
        const timespec until{time_t(left.count() / 1'000'000'000), long(left.count() % 1'000'000'000)};
        struct kevent event;
        int n = kevent(kq_, nullptr, 0, &event, 1, &until);
        if (n > 0) handle(event);
        if (n < 0 && errno != EINTR) {
            trace(now_ms(), "kevent: %s", std::strerror(errno));
            return std::nullopt;
        }
    }
    Event event = std::move(events_.front());
    events_.pop_front();
    return event;
}

void FrontDoor::handle(const struct kevent& event) {
    if (event.filter == EVFILT_READ) {
        if (int(event.ident) == listener_) accept_all();
        else drain(int(event.ident));
    } else if (event.filter == EVFILT_PROC) {
        on_exit(int32_t(event.ident));
    } else if (event.filter == EVFILT_TIMER) {
        auto it = fronts_.find(int32_t(event.ident));
        if (it == fronts_.end()) return;
        trace(now_ms(), "pid=%d still alive %lld ms after exiting: SIGKILL", it->first,
            static_cast<long long>(options_.exit_grace.count()));
        if (kill(it->first, SIGKILL) == 0) it->second.outcome.killed_by_launcher = true;
    }
}

void FrontDoor::accept_all() {
    for (;;) {
        int fd = ::accept(listener_, nullptr, nullptr);
        if (fd < 0 && errno == EINTR) continue;
        if (fd < 0) return;
        front_door::prepare_socket(fd);
        uid_t uid = 0;
        gid_t gid = 0;
        pid_t pid = 0;
        socklen_t pid_size = sizeof pid;
        if (getpeereid(fd, &uid, &gid) != 0 || uid != getuid() ||
            getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &pid_size) != 0) {
            trace(now_ms(), "refused a peer with uid %u", uid);
            close(fd);
            continue;
        }
        fcntl(fd, F_SETFL, O_NONBLOCK);
        add_filter(kq_, uintptr_t(fd), EVFILT_READ, EV_ADD, 0, 0);
        conns_[fd].pid = pid;
        trace(now_ms(), "accepted pid=%d (LOCAL_PEERPID), uid %u", pid, uid);
    }
}

void FrontDoor::drain(int fd) {
    for (;;) {
        auto it = conns_.find(fd);
        if (it == conns_.end()) return;
        char buffer[4096];
        ssize_t n = recv(fd, buffer, sizeof buffer, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n <= 0) return on_eof(fd);
        it->second.reader.append({buffer, size_t(n)});
        while (auto line = it->second.reader.next())
            if (!on_line(fd, it->second, *line)) return;
    }
}

bool FrontDoor::on_line(int fd, Conn& conn, const std::string& line) {
    if (conn.role == Conn::Role::Link) {
        events_.push_back(LinkLine{line});
        return true;
    }
    const auto message = front_door::parse_host_message(line);
    if (!message) return true;
    const int32_t pid = conn.pid;

    if (conn.role == Conn::Role::Door) {
        Front& front = fronts_.at(pid);
        std::visit(overloaded{
                       [&](const front_door::Loaded& m) { events_.push_back(FrontLoaded{pid, m.build}); },
                       [&](const front_door::Corrupt& m) { events_.push_back(FrontCorrupt{pid, m.build}); },
                       [&](const front_door::Exiting& m) {
                           trace(now_ms(), "pid=%d exiting{%d}", pid, m.code);
                           if (!front.outcome.reported_code)
                               add_filter(kq_, uintptr_t(pid), EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0,
                                          intptr_t(options_.exit_grace.count()));
                           front.outcome.reported_code = m.code;
                           front.state = front_door::FrontState::Leaving;
                       },
                       [](const auto&) {},
                   },
                   *message);
        return true;
    }

    // The first line decides what the connection is.
    if (const auto* m = std::get_if<front_door::Front>(&*message)) {
        if (!fronts_.contains(pid)) {
            on_front(fd, conn, *m);
            return true;
        }
    } else if (const auto* m = std::get_if<front_door::Attach>(&*message)) {
        auto it = fronts_.find(pid);
        if (it != fronts_.end() && it->second.state == front_door::FrontState::Assigned &&
            it->second.ticket == m->ticket && it->second.link < 0) {
            conn.role = Conn::Role::Link;
            it->second.link = fd;
            trace(now_ms(), "pid=%d attached its link (ticket %llu)", pid, static_cast<unsigned long long>(m->ticket));
            return true;
        }
    } else {
        return true;
    }
    trace(now_ms(), "pid=%d refused: %s", pid, line.c_str());
    close_conn(fd);
    return false;
}

void FrontDoor::on_front(int fd, Conn& conn, const front_door::Front& message) {
    const int32_t pid = conn.pid;
    conn.role = Conn::Role::Door;
    Front& front = fronts_[pid];
    front.door = fd;

    struct kevent change;
    EV_SET(&change, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
    front.exit_watched = kevent(kq_, &change, 1, nullptr, 0, nullptr) == 0;
    trace(now_ms(), "pid=%d front (says pid %d, recover %d, seed_pending %d); NOTE_EXIT knote: %s", pid, message.pid,
        message.recover, message.seed_pending, front.exit_watched ? "registered" : std::strerror(errno));

    front_door::RouteInput input{
        .pending_spawn = pending_ && !app_,
        .recovery_pending = false,
        .newcomer_recover = message.recover,
        .newcomer_seed_pending = message.seed_pending,
    };
    for (const auto& [other, f] : fronts_)
        if (other != pid) input.fronts.push_back({other, f.state});

    std::visit(overloaded{
                   [&](const front_door::DeliverPendingSpawn&) { assign(pid); },
                   [&](const front_door::SendActivateOther& r) {
                       trace(now_ms(), "pid=%d -> activate_other (owner pid=%d)", pid, r.owner);
                       front.state = front_door::FrontState::Leaving;
                       send(pid, front_door::ActivateOther{});
                       send(r.owner, front_door::Activate{});
                   },
                   [&](const front_door::SendQuitForInstall&) {
                       trace(now_ms(), "pid=%d -> quit_for_install", pid);
                       front.state = front_door::FrontState::Leaving;
                       send(pid, front_door::QuitForInstall{});
                       events_.push_back(QuitForInstallSent{pid});
                   },
                   [&](const auto&) {
                       trace(now_ms(), "pid=%d waiting", pid);
                       events_.push_back(FrontWaiting{pid, message.recover});
                   },
               },
               front_door::route_front(input));
}

void FrontDoor::on_eof(int fd) {
    const Conn conn = std::move(conns_.at(fd));
    close_conn(fd);
    auto it = fronts_.find(conn.pid);
    if (it == fronts_.end()) return;
    if (conn.role == Conn::Role::Link) {
        it->second.link = -1;
    } else if (conn.role == Conn::Role::Door) {
        trace(now_ms(), "pid=%d front-door EOF", conn.pid);
        it->second.door = -1;
        if (!it->second.exit_watched) finish(conn.pid);
    }
}

void FrontDoor::on_exit(int32_t pid) {
    auto it = fronts_.find(pid);
    if (it == fronts_.end()) return;
    trace(now_ms(), "pid=%d NOTE_EXIT", pid);
    // A dead process wrote everything it ever will: read it before judging.
    if (it->second.link >= 0) drain(it->second.link);
    if (it->second.door >= 0) drain(it->second.door);
    finish(pid);
}

void FrontDoor::finish(int32_t pid) {
    auto it = fronts_.find(pid);
    if (it == fronts_.end()) return;
    const Front front = std::move(it->second);
    fronts_.erase(it);
    if (front.door >= 0) close_conn(front.door);
    if (front.link >= 0) close_conn(front.link);
    if (front.outcome.reported_code) add_filter(kq_, uintptr_t(pid), EVFILT_TIMER, EV_DELETE, 0, 0);

    const ExitKind kind = front_door::classify_exit(front.outcome);
    const bool was_app = app_ == pid;
    trace(now_ms(), "pid=%d exited: %s (reported %s, killed by launcher %d)%s", pid, exit_kind_name(kind),
        front.outcome.reported_code ? std::to_string(*front.outcome.reported_code).c_str() : "nothing",
        front.outcome.killed_by_launcher, was_app ? ", the app" : "");
    events_.push_back(FrontExited{pid, kind, was_app});
    if (!was_app) return;
    app_.reset();
    if (pending_) start_pending();
}

void FrontDoor::deliver(Assignment assignment) {
    pending_ = std::move(assignment);
    pending_ticket_ = next_ticket_++;
    if (!app_) start_pending();
}

void FrontDoor::start_pending() {
    for (const auto& [pid, front] : fronts_)
        if (front.state == front_door::FrontState::Waiting) return assign(pid);
    std::vector<std::string> arguments{"--ticket=" + std::to_string(pending_ticket_)};
    arguments.insert(arguments.end(), options_.front_arguments.begin(), options_.front_arguments.end());
    launch_front(options_.host_bundle, arguments, false);
}

void FrontDoor::assign(int32_t pid) {
    Front& front = fronts_.at(pid);
    front.state = front_door::FrontState::Assigned;
    front.ticket = pending_ticket_;
    app_ = pid;
    send(pid, front_door::Assign{pending_->build, pending_->app_sha256, pending_->data_dir, pending_->backups_dir,
                                 pending_ticket_});
    trace(now_ms(), "pid=%d assign %s (ticket %llu)", pid, pending_->build.c_str(),
        static_cast<unsigned long long>(pending_ticket_));
    pending_.reset();
}

bool FrontDoor::kill_app() {
    if (!app_ || kill(*app_, SIGKILL) != 0) return false;
    fronts_.at(*app_).outcome.killed_by_launcher = true;
    trace(now_ms(), "pid=%d SIGKILL (TerminateApp)", *app_);
    return true;
}

void FrontDoor::activate_app() {
    if (app_) send(*app_, front_door::Activate{});
}

bool FrontDoor::send_link(const link::LauncherMessage& message) {
    if (!app_) return false;
    const int fd = fronts_.at(*app_).link;
    const std::string line = link::write(message);
    return fd >= 0 && ::send(fd, line.data(), line.size(), 0) == ssize_t(line.size());
}

void FrontDoor::send(int32_t pid, const front_door::LauncherMessage& message) {
    auto it = fronts_.find(pid);
    if (it == fronts_.end() || it->second.door < 0) return;
    const std::string line = front_door::write(message);
    if (::send(it->second.door, line.data(), line.size(), 0) != ssize_t(line.size()))
        trace(now_ms(), "pid=%d: could not send %s", pid, line.c_str());
}

void FrontDoor::close_conn(int fd) {
    close(fd);
    conns_.erase(fd);
}

} // namespace parties::update::launcher::macos
