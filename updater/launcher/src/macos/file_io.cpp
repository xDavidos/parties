#include "file_io.h"

#include "test_hooks.h"

#include <CommonCrypto/CommonDigest.h>
#include <copyfile.h>
#include <fcntl.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <vector>

namespace parties::update::launcher {

namespace {

std::string errno_text() { return std::string(std::strerror(errno)) + " (" + std::to_string(errno) + ")"; }

class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0) close(fd_);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }

private:
    int fd_;
};

// F_FULLFSYNC asks the drive to write its cache out. Some file systems
// (network shares, FAT) refuse it; fsync is the most they offer.
bool full_sync(int fd) { return fcntl(fd, F_FULLFSYNC) == 0 || fsync(fd) == 0; }

bool write_all(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        ssize_t n = write(fd, bytes.data(), bytes.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes.remove_prefix(size_t(n));
    }
    return true;
}

bool sync_parent(const fs::path& file, std::string& error) {
    fs::path dir = file.parent_path();
    return flush_directory(dir.empty() ? fs::path(".") : dir, error);
}

} // namespace

std::string utf8(const fs::path& path) { return path.string(); }

std::optional<std::string> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return std::nullopt;
    const std::streamoff size = in.tellg();
    if (size < 0 || size > (64ll << 20)) return std::nullopt;
    std::string bytes(size_t(size), '\0');
    in.seekg(0);
    if (!in.read(bytes.data(), size)) return std::nullopt;
    return bytes;
}

bool flush_directory(const fs::path& dir, std::string& error) {
    Fd fd(open(dir.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0 || fsync(fd.get()) != 0) {
        error = "cannot flush " + utf8(dir) + ": " + errno_text();
        return false;
    }
    return true;
}

bool durable_replace(const fs::path& file, std::string_view bytes, std::string& error) {
    fs::path tmp = file;
    tmp += ".tmp";
    {
        Fd fd(open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
        if (fd.get() < 0) {
            error = "cannot create " + utf8(tmp) + ": " + errno_text();
            return false;
        }
        if (!write_all(fd.get(), bytes) || !full_sync(fd.get())) {
            error = "cannot write " + utf8(tmp) + ": " + errno_text();
            return false;
        }
    }
    if (rename(tmp.c_str(), file.c_str()) != 0) {
        error = "cannot replace " + utf8(file) + ": " + errno_text();
        return false;
    }
    return sync_parent(file, error);
}

bool flush_file(const fs::path& path, std::string& error) {
    Fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0 || !full_sync(fd.get())) {
        error = "cannot flush " + utf8(path) + ": " + errno_text();
        return false;
    }
    return true;
}

bool durable_rename(const fs::path& from, const fs::path& to, std::string& error) {
    if (rename(from.c_str(), to.c_str()) != 0) {
        error = "cannot move " + utf8(from) + " to " + utf8(to) + ": " + errno_text();
        return false;
    }
    if (!sync_parent(to, error)) return false;
    return from.parent_path() == to.parent_path() || sync_parent(from, error);
}

bool copy_file(const fs::path& from, const fs::path& to, std::string& error) {
    if (copyfile(from.c_str(), to.c_str(), nullptr, COPYFILE_DATA) == 0) return true;
    error = "cannot copy " + utf8(from) + ": " + errno_text();
    return false;
}

bool clone_file(const fs::path& from, const fs::path& to, std::string& error) {
    if (clonefile(from.c_str(), to.c_str(), CLONE_NOFOLLOW) == 0) return true;
    if (errno != EXDEV && errno != ENOTSUP) {
        error = "cannot clone " + utf8(from) + ": " + errno_text();
        return false;
    }
    if (copyfile(from.c_str(), to.c_str(), nullptr, COPYFILE_DATA | COPYFILE_EXCL) == 0) return true;
    error = "cannot copy " + utf8(from) + ": " + errno_text();
    return false;
}

std::optional<FileDigest> digest_file(const fs::path& path) {
    Fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0) return std::nullopt;
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    std::vector<char> chunk(1 << 20);
    FileDigest digest;
    for (;;) {
        ssize_t n = read(fd.get(), chunk.data(), chunk.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return std::nullopt;
        if (n == 0) break;
        CC_SHA256_Update(&context, chunk.data(), CC_LONG(n));
        digest.size += uint64_t(n);
    }
    CC_SHA256_Final(digest.sha256.bytes.data(), &context);
    return digest;
}

bool remove_tree(const fs::path& dir, std::string& error) {
    static std::atomic<unsigned> counter{0};
    std::error_code ec;
    if (!fs::exists(dir, ec)) return true;
    fs::path trash = dir;
    if (!dir.filename().string().starts_with(".trash-")) {
        const auto now = std::chrono::system_clock::now().time_since_epoch().count();
        trash = dir.parent_path() / (".trash-" + std::to_string(now) + "-" + std::to_string(counter++) + "-" +
                                     dir.filename().string());
        if (!durable_rename(dir, trash, error)) return false;
    }
    fs::remove_all(trash, ec);
    if (ec) {
        error = "cannot delete " + utf8(trash) + ": " + ec.message();
        return false;
    }
    return true;
}

namespace testing {

void end_process(unsigned code) { _exit(int(code)); }

} // namespace testing

} // namespace parties::update::launcher
