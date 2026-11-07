#include "build_store.h"

#include "code_signing.h"
#include "file_io.h"
#include "log.h"

#include <parties/update/product.h>

#include <sys/xattr.h>

#include <algorithm>
#include <cerrno>

namespace parties::update::launcher::macos {

namespace {

template <class... F>
struct overloaded : F... {
    using F::operator()...;
};

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

// Nothing Parties downloads is expected to carry quarantine, but a file that
// does would make Gatekeeper assess the build at its first load.
void drop_quarantine(const fs::path& file) {
    if (removexattr(file.c_str(), "com.apple.quarantine", XATTR_NOFOLLOW) == 0)
        log("removed com.apple.quarantine from {}", utf8(file));
}

} // namespace

std::expected<BuildRecord, std::string> BuildStore::adopt(const fs::path& dir) const {
    auto text = read_file(dir / kBuildEntryFile);
    if (!text) return std::unexpected("no " + std::string(kBuildEntryFile) + " in " + utf8(dir));
    auto entry = parse_build_entry(*text, kMacClient);
    if (!entry)
        return std::unexpected(std::string(kBuildEntryFile) + " in " + utf8(dir) + " is unreadable: " +
                               entry.error().detail);
    if (auto bad = verify_files(dir, *entry)) return std::unexpected(*bad + " in " + utf8(dir));
    if (entry->id().dir_name() != dir.filename().string())
        return std::unexpected(utf8(dir) + " should be named " + entry->id().dir_name());
    return BuildRecord::from_entry(*entry, kMacClient);
}

std::expected<void, std::string> BuildStore::stage(const FileEntry& f, const FileSource& files,
                                                   const fs::path& dest) const {
    std::string error;
    std::error_code ec;
    auto checked = [&](const fs::path& file) -> std::expected<void, std::string> {
        auto digest = digest_file(file);
        if (!digest || digest->size != f.size || digest->sha256 != f.sha256)
            return std::unexpected(f.name + " does not match its signed entry");
        return {};
    };
    return std::visit(
        overloaded{
            [&](const FeedFiles& feed) -> std::expected<void, std::string> {
                fs::create_directories(install_.downloads(), ec);
                const fs::path partial = install_.downloads() / (f.sha256.to_hex() + ".partial");
                fs::remove(partial, ec);
                auto fail = [&](std::string why) -> std::expected<void, std::string> {
                    std::error_code ignored;
                    fs::remove(partial, ignored);
                    return std::unexpected(std::move(why));
                };
                auto fetched = feed.source.fetch_file(f, partial, feed.progress, feed.cancel);
                if (!fetched) return fail(f.name + ": " + fetched.error().detail);
                if (auto ok = checked(partial); !ok) return fail(ok.error());
                if (!durable_rename(partial, dest, error)) return fail(error);
                return {};
            },
            [&](const SeedFiles& seed) -> std::expected<void, std::string> {
                if (!clone_file(seed.frameworks / f.name, dest, error)) return std::unexpected(error);
                return checked(dest);
            },
        },
        files);
}

std::expected<BuildRecord, std::string> BuildStore::seal(const BuildEntry& entry, const FileSource& files) const {
    const fs::path final_dir = dir_of(entry.id());
    std::string error;
    std::error_code ec;
    if (fs::exists(final_dir, ec)) {
        auto bad = verify_files(final_dir, entry);
        if (!bad) return BuildRecord::from_entry(entry, kMacClient);
        log("{} is damaged ({}); sealing it again", entry.id().dir_name(), *bad);
        if (!remove_tree(final_dir, error)) return std::unexpected(error);
    }

    const fs::path staging = install_.builds() / (".staging-" + entry.id().dir_name());
    if (!remove_tree(staging, error)) return std::unexpected(error);
    fs::create_directories(staging, ec);
    if (ec) return std::unexpected("cannot create " + utf8(staging) + ": " + ec.message());
    auto fail = [&](std::string why) -> std::expected<BuildRecord, std::string> {
        std::string ignored;
        remove_tree(staging, ignored);
        return std::unexpected(std::move(why));
    };

    for (const FileEntry& f : entry.files) {
        if (auto* feed = std::get_if<FeedFiles>(&files); feed && feed->cancel.load()) return fail("cancelled");
        const fs::path dest = staging / f.name;
        if (auto staged = stage(f, files, dest); !staged) return fail(staged.error());
        drop_quarantine(dest);
        if (f.name.ends_with(".dylib")) {
            if (auto why = code_requirement_failure(dest, false)) return fail(*why);
        }
        if (!flush_file(dest, error)) return fail(error);
    }
    if (!durable_replace(staging / kBuildEntryFile, write_build_entry(entry), error)) return fail(error);
    if (!durable_rename(staging, final_dir, error)) return fail("cannot seal " + utf8(final_dir) + ": " + error);
    log("sealed {}", entry.id().dir_name());
    return BuildRecord::from_entry(entry, kMacClient);
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
        if (fs::remove_all(item.path(), removed)) log("collected {}", utf8(item.path().filename()));
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

} // namespace parties::update::launcher::macos
