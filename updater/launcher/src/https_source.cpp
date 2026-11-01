#include "file_io.h"
#include "log.h"
#include "update_source.h"

#include <format>
#include <mutex>

#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

namespace parties::update::launcher {

namespace {

constexpr int kTimeoutMs = 30000;
constexpr DWORD kChunk = 64 * 1024;

std::string error_text(const char* what) { return std::string(what) + ": " + last_error_text(); }

struct Url {
    bool https = false;
    std::wstring host;
    INTERNET_PORT port = 0;
    std::wstring path;  // with the query
};

std::optional<Url> crack(const std::wstring& text) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(text.c_str(), static_cast<DWORD>(text.size()), 0, &parts)) return std::nullopt;
    if (parts.nScheme != INTERNET_SCHEME_HTTPS && parts.nScheme != INTERNET_SCHEME_HTTP) return std::nullopt;
    Url u;
    u.https = parts.nScheme == INTERNET_SCHEME_HTTPS;
    u.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    u.port = parts.nPort;
    u.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo) u.path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (u.path.empty()) u.path = L"/";
    return u;
}

class Internet {
public:
    explicit Internet(HINTERNET h = nullptr) : h_(h) {}
    Internet(const Internet&) = delete;
    Internet& operator=(const Internet&) = delete;
    ~Internet() {
        if (h_) WinHttpCloseHandle(h_);
    }
    HINTERNET get() const { return h_; }

private:
    HINTERNET h_;
};

// Every request is a GET of a public, signed-elsewhere asset. TLS 1.2 or newer
// only, certificate errors are never ignored, and WinHTTP follows redirects
// only to https. A testing source with a loopback http base sends every URL to
// that base instead, keeping each URL's path.
class HttpsSource final : public UpdateSource {
public:
    HttpsSource(std::string base, bool loopback) : base_(std::move(base)), loopback_(loopback) {
        if (loopback_) origin_ = crack(widen(base_));
        session_ = WinHttpOpen(L"PartiesLauncher/1",
                               loopback_ ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_) {
            log("{}", error_text("WinHttpOpen"));
            return;
        }
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        if (!WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
            protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;  // Windows without TLS 1.3 in WinHTTP
            WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
        }
        DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        WinHttpSetOption(session_, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof redirects);
        WinHttpSetTimeouts(session_, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
    }
    ~HttpsSource() override {
        if (session_) WinHttpCloseHandle(session_);
    }

    std::expected<std::vector<uint8_t>, SourceError> fetch_manifest(Channel channel,
                                                                   const std::atomic<bool>& cancel) override {
        std::vector<uint8_t> out;
        auto got = get(base_ + std::string(channel_name(channel)) + ".manifest.json", kMaxEnvelopeBytes,
                       [&](const char* data, DWORD n) {
                           out.insert(out.end(), data, data + n);
                           return true;
                       },
                       cancel);
        if (!got) return std::unexpected(got.error());
        return out;
    }

    std::expected<void, SourceError> fetch_file(const FileEntry& file, const fs::path& dest,
                                                const Progress& progress, const std::atomic<bool>& cancel) override {
        std::string errors;
        for (const std::string& url : file.urls) {
            auto got = fetch_one(url, file, dest, progress, cancel);
            if (got) return {};
            std::error_code ec;
            fs::remove(dest, ec);
            if (cancel.load()) return got;
            log("download of {} from {} failed: {}", file.name, url, got.error().detail);
            errors += (errors.empty() ? "" : "; ") + got.error().detail;
        }
        return std::unexpected(SourceError{errors.empty() ? "the manifest lists no URL" : errors});
    }

    void abort() override {
        std::lock_guard lock(mutex_);
        if (request_) WinHttpCloseHandle(request_);
        request_ = nullptr;
    }

private:
    using Sink = std::function<bool(const char*, DWORD)>;

    std::expected<void, SourceError> fetch_one(const std::string& url, const FileEntry& file, const fs::path& dest,
                                               const Progress& progress, const std::atomic<bool>& cancel) {
        HANDLE out = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE) return std::unexpected(SourceError{error_text("cannot create the download")});
        Sha256Hasher hash;
        uint64_t done = 0;
        bool write_failed = false;
        auto got = get(url, file.size,
                       [&](const char* data, DWORD n) {
                           DWORD written = 0;
                           if (!hash.update(data, n) || !WriteFile(out, data, n, &written, nullptr) || written != n) {
                               write_failed = true;
                               return false;
                           }
                           done += n;
                           if (progress) progress(done);
                           return true;
                       },
                       cancel);
        CloseHandle(out);
        if (write_failed) return std::unexpected(SourceError{"cannot write the download"});
        if (!got) return got;
        if (done != file.size) return std::unexpected(SourceError{std::format("got {} of {} bytes", done, file.size)});
        if (hash.finish() != file.sha256) return std::unexpected(SourceError{"SHA-256 does not match the manifest"});
        return {};
    }

    // Streams the body of a 200 response into `sink`, failing as soon as it
    // grows past `cap` bytes.
    std::expected<void, SourceError> get(const std::string& url_text, uint64_t cap, const Sink& sink,
                                         const std::atomic<bool>& cancel) {
        if (!session_) return std::unexpected(SourceError{"no HTTP session"});
        auto url = crack(widen(url_text));
        if (!url) return std::unexpected(SourceError{"unusable URL " + url_text});
        if (loopback_) {
            if (!origin_) return std::unexpected(SourceError{"unusable feed base " + base_});
            url->https = origin_->https;
            url->host = origin_->host;
            url->port = origin_->port;
        } else if (!url->https) {
            return std::unexpected(SourceError{"refusing a non-https URL " + url_text});
        }

        Internet connection(WinHttpConnect(session_, url->host.c_str(), url->port, 0));
        if (!connection.get()) return std::unexpected(SourceError{error_text("WinHttpConnect")});
        HINTERNET request = WinHttpOpenRequest(connection.get(), L"GET", url->path.c_str(), nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               url->https ? WINHTTP_FLAG_SECURE : 0);
        if (!request) return std::unexpected(SourceError{error_text("WinHttpOpenRequest")});
        {
            std::lock_guard lock(mutex_);
            request_ = request;
        }
        auto result = exchange(request, *url, cap, sink, cancel);
        {
            std::lock_guard lock(mutex_);
            if (request_) WinHttpCloseHandle(request_);
            request_ = nullptr;
        }
        if (!result && cancel.load()) return std::unexpected(SourceError{"cancelled"});
        return result;
    }

    std::expected<void, SourceError> exchange(HINTERNET request, const Url& url, uint64_t cap, const Sink& sink,
                                              const std::atomic<bool>& cancel) {
        if (cancel.load()) return std::unexpected(SourceError{"cancelled"});
        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr))
            return std::unexpected(SourceError{error_text("request failed")});

        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status != 200) return std::unexpected(SourceError{std::format("HTTP status {}", status)});
        if (url.https && !loopback_ && !final_url_is_https(request))
            return std::unexpected(SourceError{"redirected away from https"});
        uint64_t length = 0;
        size = sizeof length;
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER64,
                                WINHTTP_HEADER_NAME_BY_INDEX, &length, &size, WINHTTP_NO_HEADER_INDEX) &&
            length > cap)
            return std::unexpected(SourceError{std::format("{} bytes is more than the {} allowed", length, cap)});

        std::vector<char> buffer(kChunk);
        uint64_t total = 0;
        for (;;) {
            if (cancel.load()) return std::unexpected(SourceError{"cancelled"});
            DWORD read = 0;
            if (!WinHttpReadData(request, buffer.data(), kChunk, &read))
                return std::unexpected(SourceError{error_text("read failed")});
            if (read == 0) return {};
            total += read;
            if (total > cap) return std::unexpected(SourceError{std::format("more than the {} bytes allowed", cap)});
            if (!sink(buffer.data(), read)) return std::unexpected(SourceError{"cannot store the download"});
        }
    }

    static bool final_url_is_https(HINTERNET request) {
        DWORD size = 0;
        WinHttpQueryOption(request, WINHTTP_OPTION_URL, nullptr, &size);
        std::wstring final_url(size / sizeof(wchar_t), L'\0');
        if (!WinHttpQueryOption(request, WINHTTP_OPTION_URL, final_url.data(), &size)) return false;
        return final_url.starts_with(L"https://");
    }

    std::string base_;
    bool loopback_;
    std::optional<Url> origin_;
    HINTERNET session_ = nullptr;
    std::mutex mutex_;
    HINTERNET request_ = nullptr;
};

} // namespace

std::unique_ptr<UpdateSource> make_https_source(std::string base, bool loopback) {
    return std::make_unique<HttpsSource>(std::move(base), loopback);
}

} // namespace parties::update::launcher
