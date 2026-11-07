#include "build_store.h"

#include "authenticode.h"
#include "build_info.h"
#include "file_io.h"
#include "log.h"

#include <algorithm>

#include <windows.h>

namespace parties::update::launcher {

ExecLease::ExecLease(ExecLease&& other) noexcept : handle_(other.handle_), exe_(std::move(other.exe_)) {
    other.handle_ = nullptr;
}

ExecLease::~ExecLease() {
    if (handle_) CloseHandle(handle_);
}

namespace {

std::optional<std::string> verify_files(const fs::path& dir, const BuildEntry& entry) {
    for (const FileEntry& f : entry.files) {
        auto digest = digest_file(dir / f.name);
        if (!digest) return "missing " + f.name;
        if (digest->size != f.size || digest->sha256 != f.sha256) return "modified " + f.name;
    }
    return std::nullopt;
}

bool needed(const SupervisorState& s, const BuildId& id) {
    if (id == s.good || s.previous_good == id) return true;
    if (auto* staged = std::get_if<Staged>(&s.activation); staged && staged->build == id) return true;
    if (auto* p = std::get_if<Probation>(&s.activation); p && (p->candidate == id || p->fallback == id)) return true;
    return false;
}

} // namespace

std::expected<BuildRecord, std::string> BuildStore::adopt(const fs::path& dir) const {
    auto text = read_file(dir / kBuildInfoFile);
    if (!text) return std::unexpected("no " + std::string(kBuildInfoFile) + " in " + utf8(dir));
    auto entry = parse_build_info(*text);
    if (!entry) return std::unexpected(std::string(kBuildInfoFile) + " in " + utf8(dir) + " is unreadable");
    if (auto bad = verify_files(dir, *entry)) return std::unexpected(*bad + " in " + utf8(dir));
    if (entry->id().dir_name() != dir.filename().string())
        return std::unexpected(utf8(dir) + " should be named " + entry->id().dir_name());
    return BuildRecord::from_entry(*entry, kWindowsClient);
}

std::expected<fs::path, std::string> BuildStore::download(const FileEntry& f, UpdateSource& source,
                                                          const Progress& progress,
                                                          const std::atomic<bool>& cancel) const {
    std::error_code ec;
    fs::create_directories(install_.downloads(), ec);
    const fs::path partial = install_.downloads() / (f.sha256.to_hex() + ".partial");
    fs::remove(partial, ec);
    auto fail = [&](std::string why) -> std::expected<fs::path, std::string> {
        std::error_code ignored;
        fs::remove(partial, ignored);
        return std::unexpected(std::move(why));
    };
    auto fetched = source.fetch_file(f, partial, progress, cancel);
    if (!fetched) return fail(f.name + ": " + fetched.error().detail);
    auto digest = digest_file(partial);
    if (!digest || digest->size != f.size || digest->sha256 != f.sha256)
        return fail(f.name + " does not match the signed manifest");
    if (f.name.ends_with(".exe") && !pinned_publisher().empty()) {
        if (auto why = publisher_mismatch(partial, pinned_publisher())) return fail(f.name + ": " + *why);
    }
    std::string error;
    if (!flush_file(partial, error)) return fail(error);
    return partial;
}

// The directory may hold the running launcher, so it cannot be renamed aside;
// putting the signed bytes back file by file restores what its name promises.
std::expected<BuildRecord, std::string> BuildStore::repair(const BuildEntry& entry, UpdateSource& source,
                                                           const Progress& progress,
                                                           const std::atomic<bool>& cancel) const {
    const fs::path dir = dir_of(entry.id());
    uint64_t total = 0;
    for (const FileEntry& f : entry.files) {
        auto digest = digest_file(dir / f.name);
        if (digest && digest->size == f.size && digest->sha256 == f.sha256) continue;
        if (cancel.load()) return std::unexpected("cancelled");
        auto partial = download(f, source, [&](uint64_t done) { if (progress) progress(total + done); }, cancel);
        if (!partial) return std::unexpected(partial.error());
        const fs::path dest = dir / f.name;
        if (!MoveFileExW(partial->c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::string why = "cannot replace " + utf8(dest) + ": " + last_error_text();
            std::error_code ec;
            fs::remove(*partial, ec);
            return std::unexpected(why);
        }
        log("repaired {}", utf8(dest));
        total += f.size;
    }
    const std::string info = write_build_info(entry);
    std::string error;
    if (read_file(dir / kBuildInfoFile) != info && !durable_replace(dir / kBuildInfoFile, info, error))
        return std::unexpected(error);
    if (auto bad = verify_files(dir, entry)) return std::unexpected("repair of " + utf8(dir) + " failed: " + *bad);
    return BuildRecord::from_entry(entry, kWindowsClient);
}

std::expected<BuildRecord, std::string> BuildStore::seal(const BuildEntry& entry, UpdateSource& source,
                                                         const Progress& progress,
                                                         const std::atomic<bool>& cancel) const {
    const fs::path final_dir = dir_of(entry.id());
    std::error_code ec;
    if (fs::exists(final_dir, ec)) {
        if (!verify_files(final_dir, entry)) return BuildRecord::from_entry(entry, kWindowsClient);
        return repair(entry, source, progress, cancel);
    }

    const fs::path staging = install_.builds() / (".staging-" + entry.id().dir_name());
    std::string error;
    if (!remove_tree(staging, error)) return std::unexpected(error);
    fs::create_directories(staging, ec);
    if (ec) return std::unexpected("cannot create " + utf8(staging) + ": " + ec.message());

    auto fail = [&](std::string why) -> std::expected<BuildRecord, std::string> {
        std::string ignored;
        remove_tree(staging, ignored);
        return std::unexpected(std::move(why));
    };

    uint64_t total = 0;
    for (const FileEntry& f : entry.files) {
        if (cancel.load()) return fail("cancelled");
        auto partial = download(f, source, [&](uint64_t done) { if (progress) progress(total + done); }, cancel);
        if (!partial) return fail(partial.error());
        const fs::path dest = staging / f.name;
        if (!MoveFileExW(partial->c_str(), dest.c_str(), MOVEFILE_WRITE_THROUGH)) {
            std::string why = "cannot move " + f.name + " into " + utf8(staging) + ": " + last_error_text();
            fs::remove(*partial, ec);
            return fail(why);
        }
        total += f.size;
    }
    if (!durable_replace(staging / kBuildInfoFile, write_build_info(entry), error)) return fail(error);
    if (!MoveFileExW(staging.c_str(), final_dir.c_str(), MOVEFILE_WRITE_THROUGH))
        return fail("cannot seal " + utf8(final_dir) + ": " + last_error_text());
    log("sealed {}", entry.id().dir_name());
    return BuildRecord::from_entry(entry, kWindowsClient);
}

std::optional<ExecLease> BuildStore::lease_app(const BuildRecord& record) const {
    fs::path exe = dir_of(record.id) / kWindowsClient.app_file;
    HANDLE h = CreateFileW(exe.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        log("cannot lease {}: {}", utf8(exe), last_error_text());
        return std::nullopt;
    }
    ExecLease lease(h, exe);
    auto digest = digest_handle(h);
    if (!digest || digest->sha256 != record.app_sha256) {
        log("{} does not match its recorded digest", utf8(exe));
        return std::nullopt;
    }
    return lease;
}

bool BuildStore::remove(const BuildId& id, const SupervisorState& committed, std::string& error) const {
    if (needed(committed, id)) {
        error = id.dir_name() + " is still needed";
        return false;
    }
    if (!remove_tree(dir_of(id), error)) return false;
    log("deleted {}", id.dir_name());
    return true;
}

void BuildStore::collect_garbage(const SupervisorState& committed, const fs::path& keep_dir) const {
    std::error_code ec;
    for (const auto& item : fs::directory_iterator(install_.downloads(), ec)) {
        std::error_code removed;
        if (fs::remove(item.path(), removed)) log("collected {}", utf8(item.path().filename()));
    }
    for (const auto& item : fs::directory_iterator(install_.builds(), ec)) {
        if (!item.is_directory(ec)) continue;
        const std::string name = item.path().filename().string();
        bool tracked = std::ranges::any_of(committed.builds,
                                           [&](const BuildRecord& r) { return r.id.dir_name() == name; });
        bool own = fs::equivalent(item.path(), keep_dir, ec);
        if (tracked || own) continue;
        std::string error;
        if (remove_tree(item.path(), error)) log("collected {}", name);
        else log("cannot collect {}: {}", name, error);
    }
}

} // namespace parties::update::launcher
