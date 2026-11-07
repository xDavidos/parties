// The launcher's end of the macOS front-door protocol (process-contract.md),
// and the executor of Spawn, TerminateApp and activation. Owns every front
// connection and the app's link connection. A front's parent is launchd, so
// its death is the kernel's EOF on its connection plus EVFILT_PROC NOTE_EXIT,
// and its exit code is the last exiting{code} its host wrote.

#pragma once

#include <parties/update/front_door.h>
#include <parties/update/front_door_policy.h>
#include <parties/update/link.h>
#include <parties/update/sha256_digest.h>

#include <sys/event.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace parties::update::launcher::macos {

// Asks LaunchServices to start the host bundle with these arguments. Returns
// at once; the front announces itself on the front door. What LaunchServices
// answered is logged.
void launch_front(const std::string& bundle, const std::vector<std::string>& arguments, bool new_instance);

const char* exit_kind_name(ExitKind kind);

struct Assignment {
    std::string build;  // "builds\<dir>"
    Sha256 app_sha256;
    std::string data_dir;
    std::string backups_dir;
};

// A front connected and nothing was pending for it: the supervisor decides.
struct FrontWaiting {
    int32_t pid = 0;
    bool recover = false;
};
struct FrontLoaded {
    int32_t pid = 0;
    std::string build;
};
struct FrontCorrupt {
    int32_t pid = 0;
    std::string build;
};
// One line from the app's link connection, unparsed.
struct LinkLine {
    int32_t pid = 0;
    std::string line;
};
struct FrontExited {
    int32_t pid = 0;
    ExitKind kind = ExitKind::Abnormal;
    bool was_app = false;
};
// A front with a newer seed was told quit_for_install: the launcher exits next.
struct QuitForInstallSent {
    int32_t pid = 0;
};
using Event = std::variant<FrontWaiting, FrontLoaded, FrontCorrupt, LinkLine, FrontExited, QuitForInstallSent>;

class FrontDoor {
public:
    struct Options {
        std::string socket_path;
        std::string host_bundle;                   // the bundle this launcher runs from
        std::vector<std::string> front_arguments;  // after --ticket=<n> in every launch
        std::chrono::milliseconds exit_grace{30'000};
    };

    // Binds the socket, replacing a stale one. The caller holds launcher.lock.
    // nullptr when the socket cannot be bound.
    static std::unique_ptr<FrontDoor> listen(Options options);
    ~FrontDoor();
    FrontDoor(const FrontDoor&) = delete;
    FrontDoor& operator=(const FrontDoor&) = delete;

    // The next event, or nullopt when `timeout` passes first. Looks at the
    // kernel at least once, so a zero timeout polls.
    std::optional<Event> next(std::chrono::milliseconds timeout);
    // A kqueue that is readable while next() has something to return, for a
    // loop that waits on more than the front door.
    int fd() const { return kq_; }

    // Spawn: assign to a waiting front, else launch one through LaunchServices
    // once the previous app front has exited, and assign when it connects.
    void deliver(Assignment assignment);
    // TerminateApp: SIGKILL the app front. False when there is none.
    bool kill_app();
    // A Spawn whose front never connected is given up: a front that comes
    // later is not handed it.
    void withdraw() { pending_.reset(); }
    std::optional<int32_t> app_pid() const { return app_; }
    // The user's session is ending: a front that dies from now on without
    // reporting a code ended with it, not by crashing.
    void session_ending();
    // One line to the app on its link. False when it has no link.
    bool send_link(const link::LauncherMessage& message);
    void activate_app();

private:
    struct Conn {
        enum class Role : uint8_t { New, Door, Link };
        int32_t pid = 0;
        Role role = Role::New;
        link::LineReader reader;
    };
    struct Front {
        int door = -1;
        int link = -1;
        front_door::FrontState state = front_door::FrontState::Waiting;
        uint64_t ticket = 0;
        bool exit_watched = false;
        front_door::FrontOutcome outcome;
    };

    FrontDoor(Options options, int kq, int listener);
    void handle(const struct kevent& event);
    void accept_all();
    void drain(int fd);
    bool on_line(int fd, Conn& conn, const std::string& line);
    void on_front(int fd, Conn& conn, const front_door::Front& message);
    void on_eof(int fd);
    void on_exit(int32_t pid);
    void finish(int32_t pid);
    void assign(int32_t pid);
    void start_pending();
    void send(int32_t pid, const front_door::LauncherMessage& message);
    void close_conn(int fd);
    double now_ms() const;

    Options options_;
    int kq_;
    int listener_;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::map<int, Conn> conns_;       // by descriptor
    std::map<int32_t, Front> fronts_;  // by pid: one front-door connection per process
    std::optional<int32_t> app_;
    std::optional<Assignment> pending_;
    uint64_t pending_ticket_ = 0;
    uint64_t next_ticket_ = 1;
    std::deque<Event> events_;
};

} // namespace parties::update::launcher::macos
