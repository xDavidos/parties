// data.lock in the data directory. A front holds it from just before it loads
// a payload until it dies; the launcher takes it around every read, snapshot
// and restore of the live database. The kernel drops it when its holder dies,
// so nothing restores under a live app. Shared by the host and the launcher.

#pragma once

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <filesystem>
#include <optional>
#include <utility>

namespace parties::update {

class DataLock {
public:
    // LOCK_EX | LOCK_NB. Creates the data directory and the lock file if needed.
    static std::optional<DataLock> try_acquire(const std::filesystem::path& data_dir) {
        std::error_code ignored;
        std::filesystem::create_directories(data_dir, ignored);
        // Close-on-exec: a child that inherited the descriptor would hold the
        // lock after its front died.
        int fd = open((data_dir / "data.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) return std::nullopt;
        if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
            close(fd);
            return std::nullopt;
        }
        return DataLock(fd);
    }

    DataLock(DataLock&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    DataLock& operator=(DataLock&&) = delete;
    ~DataLock() {
        if (fd_ >= 0) close(fd_);
    }

private:
    explicit DataLock(int fd) : fd_(fd) {}
    int fd_;
};

} // namespace parties::update
