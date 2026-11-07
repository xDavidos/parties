#include "link_server.h"

#include "file_io.h"
#include "log.h"

#include <windows.h>

namespace parties::update::launcher {

std::expected<std::unique_ptr<LinkServer>, std::string> LinkServer::create(const std::wstring& name) {
    HANDLE pipe = CreateNamedPipeW(name.c_str(),
                                   PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                   64 * 1024, 64 * 1024, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return std::unexpected("cannot create the link pipe: " + last_error_text());
    std::unique_ptr<LinkServer> server(
        new LinkServer(pipe, CreateEventW(nullptr, TRUE, FALSE, nullptr), CreateEventW(nullptr, TRUE, FALSE, nullptr)));
    auto* ov = new OVERLAPPED{};
    ov->hEvent = server->read_event_;
    server->overlapped_ = ov;
    if (!ConnectNamedPipe(pipe, ov)) {
        DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED) SetEvent(server->read_event_);
        else if (err != ERROR_IO_PENDING) return std::unexpected("cannot listen on the link pipe: " + last_error_text());
    }
    return server;
}

LinkServer::~LinkServer() {
    if (pipe_) {
        CancelIoEx(pipe_, nullptr);
        if (overlapped_) {
            DWORD ignored = 0;
            GetOverlappedResult(pipe_, static_cast<OVERLAPPED*>(overlapped_), &ignored, TRUE);
        }
        CloseHandle(pipe_);
    }
    delete static_cast<OVERLAPPED*>(overlapped_);
    if (read_event_) CloseHandle(read_event_);
    if (write_event_) CloseHandle(write_event_);
}

void LinkServer::fail(const char* why) {
    if (state_ != State::Broken) log("link closed: {}", why);
    state_ = State::Broken;
    ResetEvent(read_event_);
    DisconnectNamedPipe(pipe_);
}

bool LinkServer::start_read() {
    auto* ov = static_cast<OVERLAPPED*>(overlapped_);
    *ov = OVERLAPPED{};
    ov->hEvent = read_event_;
    if (ReadFile(pipe_, buffer_, sizeof buffer_, nullptr, ov)) return true;
    if (GetLastError() == ERROR_IO_PENDING) return true;
    fail("read failed");
    return false;
}

std::vector<link::AppMessage> LinkServer::poll() {
    std::vector<link::AppMessage> out;
    auto* ov = static_cast<OVERLAPPED*>(overlapped_);
    while (state_ != State::Broken) {
        DWORD bytes = 0;
        if (!GetOverlappedResult(pipe_, ov, &bytes, FALSE)) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_INCOMPLETE) break;
            if (!(state_ == State::Connecting && err == ERROR_PIPE_CONNECTED)) {
                fail(state_ == State::Connecting ? "connect failed" : "peer disconnected");
                break;
            }
        }
        if (state_ == State::Connecting) {
            ULONG pid = 0;
            if (!GetNamedPipeClientProcessId(pipe_, &pid) || (client_pid_ && pid != client_pid_)) {
                fail("a process other than the app connected");
                break;
            }
            state_ = State::Reading;
        } else {
            reader_.append({buffer_, bytes});
            while (auto line = reader_.next()) {
                if (auto message = link::parse_app_message(*line)) out.push_back(std::move(*message));
            }
        }
        if (!start_read()) break;
    }
    return out;
}

bool LinkServer::send(const link::LauncherMessage& message) {
    if (state_ != State::Reading) return false;
    std::string line = link::write(message);
    OVERLAPPED ov{};
    ov.hEvent = write_event_;
    DWORD written = 0;
    if (!WriteFile(pipe_, line.data(), static_cast<DWORD>(line.size()), nullptr, &ov) &&
        GetLastError() != ERROR_IO_PENDING) {
        fail("write failed");
        return false;
    }
    if (WaitForSingleObject(write_event_, 2000) != WAIT_OBJECT_0) {
        CancelIoEx(pipe_, &ov);
        GetOverlappedResult(pipe_, &ov, &written, TRUE);
        fail("the app stopped reading");
        return false;
    }
    if (!GetOverlappedResult(pipe_, &ov, &written, FALSE) || written != line.size()) {
        fail("write failed");
        return false;
    }
    return true;
}

} // namespace parties::update::launcher
