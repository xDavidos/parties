#pragma once

// The launcher end of the app link (contracts.md): one overlapped named pipe
// per app process, created before the spawn so the app can connect at once.

#include <parties/update/link.h>

#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace parties::update::launcher {

class LinkServer {
public:
    // Not movable: a pending read points into the object.
    static std::expected<std::unique_ptr<LinkServer>, std::string> create(const std::wstring& name);

    LinkServer(const LinkServer&) = delete;
    LinkServer& operator=(const LinkServer&) = delete;
    ~LinkServer();

    // Signalled when a connect or read completes; then call poll().
    void* event() const { return read_event_; }

    // Only this process may talk to us; anything else is disconnected.
    void expect_client(unsigned long pid) { client_pid_ = pid; }

    // Messages that arrived since the last poll. A peer that disconnects or
    // misbehaves leaves the link broken; that is never a crash signal.
    std::vector<link::AppMessage> poll();

    bool send(const link::LauncherMessage& message);
    bool connected() const { return state_ == State::Reading; }

private:
    enum class State { Connecting, Reading, Broken };

    LinkServer(void* pipe, void* read_event, void* write_event)
        : pipe_(pipe), read_event_(read_event), write_event_(write_event) {}
    bool start_read();
    void fail(const char* why);

    void* pipe_;
    void* read_event_;
    void* write_event_;
    void* overlapped_ = nullptr;  // OVERLAPPED for connect and reads
    State state_ = State::Connecting;
    unsigned long client_pid_ = 0;
    char buffer_[4096];
    link::LineReader reader_;
};

} // namespace parties::update::launcher
