#include <client/verification.h>

#include <parties/log.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <share.h>

#include <chrono>

namespace parties::client {

std::optional<std::string> parse_verification_options(int argc, char** argv,
                                                      VerificationOptions& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--automation-pipe") {
            if (!has_value) return "--automation-pipe needs a name";
            options.automation_pipe = argv[++i];
        } else if (arg == "--verify-log") {
            if (!has_value) return "--verify-log needs a path";
            options.log_path = std::filesystem::path(argv[++i]);
        } else if (arg == "--test-pattern") {
            options.test_pattern = true;
        } else if (arg == "--test-pattern-noise") {
            options.pattern_noise = true;
        } else if (arg == "--test-pattern-size") {
            if (!has_value) return "--test-pattern-size needs WIDTHxHEIGHT";
            unsigned width = 0, height = 0;
            if (sscanf_s(argv[++i], "%ux%u", &width, &height) != 2 ||
                width < 64 || height < 64 || width > 7680 || height > 4320)
                return "--test-pattern-size must be WIDTHxHEIGHT between 64x64 and 7680x4320";
            options.pattern_width = width & ~1u;
            options.pattern_height = height & ~1u;
        }
    }
    return std::nullopt;
}

int64_t qpc_100ns() {
    static const int64_t frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart / frequency) * 10'000'000 + (now.QuadPart % frequency) * 10'000'000 / frequency;
}

std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out += buffer;
            } else {
                out += c;
            }
        }
    }
    return out;
}

VerificationLog::~VerificationLog() {
    if (file_) std::fclose(file_);
}

bool VerificationLog::open(const std::filesystem::path& path) {
    std::error_code error;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
    file_ = _wfsopen(path.c_str(), L"ab", _SH_DENYNO);
    if (!file_) {
        LOG_ERROR("Could not open verification log {}", path.string());
        return false;
    }
    LOG_INFO("Writing verification events to {}", path.string());
    return true;
}

void VerificationLog::write(const std::string& line) {
    std::lock_guard lock(mutex_);
    if (!file_) return;
    std::fwrite(line.data(), 1, line.size(), file_);
    std::fputc('\n', file_);
    std::fflush(file_);
}

AutomationServer::~AutomationServer() {
    stop();
}

bool AutomationServer::start(const std::string& name) {
    if (thread_.joinable() || name.empty()) return false;
    const std::string path = "\\\\.\\pipe\\" + name;
    pipe_path_.assign(path.begin(), path.end());
    stopping_ = false;
    thread_ = std::thread([this] { serve(); });
    LOG_INFO("Automation pipe listening on {}", path);
    return true;
}

void AutomationServer::stop() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        for (auto& pending : pending_)
            pending.response.set_value(R"({"ok":false,"error":"shutting down"})");
        pending_.clear();
    }
    const HANDLE thread = thread_.native_handle();
    while (WaitForSingleObject(thread, 0) == WAIT_TIMEOUT) {
        CancelSynchronousIo(thread);
        const HANDLE client = CreateFileW(pipe_path_.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                          nullptr, OPEN_EXISTING, 0, nullptr);
        if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
        Sleep(5);
    }
    thread_.join();
}

void AutomationServer::pump(const Handler& handler) {
    std::vector<Pending> work;
    {
        std::lock_guard lock(mutex_);
        work.swap(pending_);
    }
    for (auto& pending : work)
        pending.response.set_value(handler(pending.request));
}

std::string AutomationServer::dispatch(std::string request) {
    std::future<std::string> response;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return R"({"ok":false,"error":"shutting down"})";
        pending_.push_back({std::move(request), {}});
        response = pending_.back().response.get_future();
    }
    if (response.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
        return R"({"ok":false,"error":"the UI thread did not answer within 10 s"})";
    return response.get();
}

void AutomationServer::serve() {
    for (;;) {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return;
        }
        const HANDLE pipe = CreateNamedPipeW(
            pipe_path_.c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 64 * 1024, 64 * 1024, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            LOG_ERROR("Automation pipe creation failed ({})", GetLastError());
            return;
        }
        const BOOL connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        std::string buffer;
        char chunk[4096];
        DWORD read = 0;
        while (connected && ReadFile(pipe, chunk, sizeof(chunk), &read, nullptr) && read > 0) {
            buffer.append(chunk, read);
            size_t newline;
            while ((newline = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, newline);
                buffer.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                std::string response = dispatch(std::move(line)) + "\n";
                DWORD written = 0;
                if (!WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()), &written, nullptr))
                    break;
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

}
