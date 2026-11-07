#include "file_io.h"

#include <atomic>
#include <system_error>
#include <vector>

#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace parties::update::launcher {

std::string utf8(const fs::path& path) {
    const auto u8 = path.u8string();
    return {reinterpret_cast<const char*>(u8.data()), u8.size()};
}

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string last_error_text() {
    DWORD code = GetLastError();
    return std::system_category().message(static_cast<int>(code)) + " (" + std::to_string(code) + ")";
}

fs::path self_exe() {
    std::wstring buffer(32768, L'\0');
    buffer.resize(GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size())));
    return buffer;
}

namespace {

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~Handle() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};

bool write_all(HANDLE h, std::string_view bytes) {
    while (!bytes.empty()) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size(), 1 << 20));
        DWORD written = 0;
        if (!WriteFile(h, bytes.data(), chunk, &written, nullptr) || written == 0) return false;
        bytes.remove_prefix(written);
    }
    return true;
}

} // namespace

Sha256Hasher::Sha256Hasher() {
    if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return;
    if (BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0) != 0) hash_ = nullptr;
}

Sha256Hasher::~Sha256Hasher() {
    if (hash_) BCryptDestroyHash(hash_);
    if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
}

bool Sha256Hasher::update(const void* data, unsigned long size) {
    return BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(data)), size, 0) == 0;
}

std::optional<Sha256> Sha256Hasher::finish() {
    Sha256 out;
    if (BCryptFinishHash(hash_, out.bytes.data(), static_cast<ULONG>(out.bytes.size()), 0) != 0) return std::nullopt;
    return out;
}

std::optional<std::string> read_file(const fs::path& path) {
    Handle f{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (f.h == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f.h, &size) || size.QuadPart > (64ll << 20)) return std::nullopt;
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    size_t done = 0;
    while (done < bytes.size()) {
        DWORD read = 0;
        if (!ReadFile(f.h, bytes.data() + done, static_cast<DWORD>(bytes.size() - done), &read, nullptr) || read == 0)
            return std::nullopt;
        done += read;
    }
    return bytes;
}

bool durable_replace(const fs::path& file, std::string_view bytes, std::string& error) {
    fs::path tmp = file;
    tmp += ".tmp";
    {
        Handle f{CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (f.h == INVALID_HANDLE_VALUE) {
            error = "cannot create " + utf8(tmp) + ": " + last_error_text();
            return false;
        }
        if (!write_all(f.h, bytes) || !FlushFileBuffers(f.h)) {
            error = "cannot write " + utf8(tmp) + ": " + last_error_text();
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "cannot replace " + utf8(file) + ": " + last_error_text();
        return false;
    }
    return true;
}

bool flush_file(const fs::path& path, std::string& error) {
    Handle f{CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (f.h == INVALID_HANDLE_VALUE || !FlushFileBuffers(f.h)) {
        error = "cannot flush " + utf8(path) + ": " + last_error_text();
        return false;
    }
    return true;
}

std::optional<FileDigest> digest_handle(void* handle) {
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN)) return std::nullopt;
    Sha256Hasher hasher;
    if (!hasher.ok()) return std::nullopt;
    std::vector<char> buffer(1 << 20);
    FileDigest digest;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) return std::nullopt;
        if (read == 0) break;
        if (!hasher.update(buffer.data(), read)) return std::nullopt;
        digest.size += read;
    }
    auto sha = hasher.finish();
    if (!sha) return std::nullopt;
    digest.sha256 = *sha;
    return digest;
}

std::optional<FileDigest> digest_file(const fs::path& path) {
    Handle f{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (f.h == INVALID_HANDLE_VALUE) return std::nullopt;
    return digest_handle(f.h);
}

std::optional<FileDigest> digest_bytes(std::span<const uint8_t> bytes) {
    Sha256Hasher hasher;
    if (!hasher.ok()) return std::nullopt;
    FileDigest digest{bytes.size(), {}};
    while (!bytes.empty()) {
        ULONG chunk = static_cast<ULONG>(std::min<size_t>(bytes.size(), 1u << 30));
        if (!hasher.update(bytes.data(), chunk)) return std::nullopt;
        bytes = bytes.subspan(chunk);
    }
    auto sha = hasher.finish();
    if (!sha) return std::nullopt;
    digest.sha256 = *sha;
    return digest;
}

bool remove_tree(const fs::path& dir, std::string& error) {
    static std::atomic<unsigned> counter{0};
    std::error_code ec;
    if (!fs::exists(dir, ec)) return true;
    fs::path trash = dir.parent_path() /
                     (".trash-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) +
                      "-" + std::to_string(counter++) + "-" + dir.filename().string());
    if (dir.filename().string().starts_with(".trash-")) {
        trash = dir;
    } else if (!MoveFileExW(dir.c_str(), trash.c_str(), MOVEFILE_WRITE_THROUGH)) {
        error = "cannot move " + utf8(dir) + " aside: " + last_error_text();
        return false;
    }
    fs::remove_all(trash, ec);
    if (ec) {
        error = "cannot delete " + utf8(trash) + ": " + ec.message();
        return false;
    }
    return true;
}

} // namespace parties::update::launcher
