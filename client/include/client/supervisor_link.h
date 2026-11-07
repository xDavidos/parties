#pragma once

// Message-thread only: every call is non-blocking.

#include <parties/update/link.h>

#include <memory>
#include <optional>
#include <span>
#include <string>

namespace parties::client {

class SupervisorLink {
public:
    // nullptr when the command line has no --supervisor-pipe (unmanaged).
    // Otherwise connects and sends hello; a link that cannot connect still
    // counts as managed, because the launcher owns updates either way.
    static std::unique_ptr<SupervisorLink> from_args(std::span<const std::wstring> args,
                                                     const update::Version& version);
    ~SupervisorLink();
    SupervisorLink(const SupervisorLink&) = delete;
    SupervisorLink& operator=(const SupervisorLink&) = delete;

    bool poll();
    const std::optional<update::link::Status>& status() const { return status_; }
    bool exit_requested() const { return exit_requested_; }

    void report_healthy(uint32_t data_schema);

    void request(const update::link::Request& request);

private:
    explicit SupervisorLink(void* pipe) : pipe_(pipe) {}
    void send(const update::link::AppMessage& message);

    void* pipe_;
    update::link::LineReader reader_;
    std::optional<uint32_t> protocol_;
    std::optional<update::link::Status> status_;
    bool exit_requested_ = false;
    bool healthy_reported_ = false;
};

} // namespace parties::client
