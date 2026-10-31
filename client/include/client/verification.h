#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace parties::client {

// Developer-build hooks used by the stream verification harness. Retail
// builds never parse these flags, so every option stays at its default.
struct VerificationOptions {
    std::string automation_pipe;
    std::filesystem::path log_path;
    bool test_pattern = false;
    uint32_t pattern_width = 1280;
    uint32_t pattern_height = 720;
    bool pattern_noise = false;
};

// Returns an error message when a recognised flag is malformed.
std::optional<std::string> parse_verification_options(int argc, char** argv,
                                                      VerificationOptions& options);

// QueryPerformanceCounter in 100 ns units, the clock Windows.Graphics.Capture
// stamps composed frames with, so sender, viewer and probe logs line up.
int64_t qpc_100ns();

std::string json_escape(std::string_view text);

class VerificationLog {
public:
    ~VerificationLog();
    bool open(const std::filesystem::path& path);
    explicit operator bool() const { return file_ != nullptr; }
    // Appends one JSON object line and flushes it, so a killed process keeps its evidence.
    void write(const std::string& line);

private:
    std::mutex mutex_;
    FILE* file_ = nullptr;
};

// Newline-delimited JSON requests over \\.\pipe\<name>. Requests are handed to
// pump() on the UI message thread, which owns RmlUi input.
class AutomationServer {
public:
    using Handler = std::function<std::string(const std::string& request)>;

    ~AutomationServer();
    bool start(const std::string& name);
    void stop();
    void pump(const Handler& handler);

private:
    struct Pending {
        std::string request;
        std::promise<std::string> response;
    };

    void serve();
    std::string dispatch(std::string request);

    std::wstring pipe_path_;
    std::thread thread_;
    std::mutex mutex_;
    std::vector<Pending> pending_;
    bool stopping_ = false;
};

}
