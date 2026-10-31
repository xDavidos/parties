#include <client/supervisor_link.h>

#include <parties/log.h>

#include <chrono>
#include <string_view>
#include <thread>

#include <windows.h>

namespace parties::client {

namespace {

constexpr std::wstring_view kFlag = L"--supervisor-pipe=";

} // namespace

std::unique_ptr<SupervisorLink> SupervisorLink::from_args(std::span<const std::wstring> args,
                                                          const update::Version& version) {
    std::wstring name;
    for (const std::wstring& arg : args)
        if (arg.starts_with(kFlag)) name = arg.substr(kFlag.size());
    if (name.empty()) return nullptr;

    HANDLE pipe = INVALID_HANDLE_VALUE;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::unique_ptr<SupervisorLink> link(new SupervisorLink(pipe));
    if (pipe == INVALID_HANDLE_VALUE) {
        LOG_WARN("Cannot connect to the launcher link (error {}); updates stay with the launcher", GetLastError());
        return link;
    }
    LOG_INFO("Supervised by the Parties launcher");
    link->send(update::link::Hello{update::kLinkProtocol, update::kMinLauncherLink, version});
    return link;
}

SupervisorLink::~SupervisorLink() {
    if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
}

void SupervisorLink::send(const update::link::AppMessage& message) {
    if (pipe_ == INVALID_HANDLE_VALUE) return;
    const std::string line = update::link::write(message);
    DWORD written = 0;
    if (!WriteFile(pipe_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr)) {
        LOG_WARN("The launcher link closed (error {})", GetLastError());
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
}

bool SupervisorLink::poll() {
    bool changed = false;
    while (pipe_ != INVALID_HANDLE_VALUE) {
        while (auto line = reader_.next()) {
            auto message = update::link::parse_launcher_message(*line);
            if (!message) continue;
            if (auto* welcome = std::get_if<update::link::Welcome>(&*message)) {
                protocol_ = welcome->link_protocol;
            } else if (auto* status = std::get_if<update::link::Status>(&*message)) {
                if (status_ != *status) {
                    status_ = *status;
                    changed = true;
                }
            } else if (std::holds_alternative<update::link::ExitRequested>(*message)) {
                LOG_INFO("The launcher asked the app to exit");
                exit_requested_ = true;
            }
        }
        DWORD available = 0;
        if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr)) {
            LOG_WARN("The launcher link closed (error {})", GetLastError());
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
            break;
        }
        if (available == 0) break;
        char buffer[4096];
        DWORD read = 0;
        if (!ReadFile(pipe_, buffer, available < sizeof buffer ? available : sizeof buffer, &read, nullptr)) break;
        reader_.append({buffer, read});
    }
    return changed;
}

void SupervisorLink::report_healthy(uint32_t data_schema) {
    if (healthy_reported_) return;
    healthy_reported_ = true;
    LOG_INFO("Reporting healthy to the launcher (data schema {})", data_schema);
    send(update::link::Healthy{data_schema});
}

void SupervisorLink::request(const update::link::Request& request) { send(request); }

} // namespace parties::client
