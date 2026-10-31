#include "legacy_bridge.h"

#include "file_io.h"
#include "log.h"
#include "shell_integration.h"
#include "testing.h"

#include <client/legacy_import.h>

#include <algorithm>

#include <windows.h>
#include <tlhelp32.h>

namespace parties::update::launcher {

namespace {

bool same_name(const std::wstring& a, const wchar_t* b) { return _wcsicmp(a.c_str(), b) == 0; }

std::vector<std::wstring> portable_images() {
    if (auto image = testing::legacy_image()) return {*image};
    if (testing::enabled()) return {};
    return {L"parties_client.exe", L"parties_client.old.exe"};
}

std::vector<fs::path> portable_process_dirs() {
    const std::vector<std::wstring> images = portable_images();
    std::vector<fs::path> dirs;
    if (images.empty()) return dirs;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return dirs;
    PROCESSENTRY32W entry{sizeof entry};
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == GetCurrentProcessId()) continue;
        bool portable = false;
        for (const std::wstring& image : images) portable |= same_name(image, entry.szExeFile);
        if (!portable) continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process) continue;
        std::wstring path(32768, L'\0');
        DWORD size = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(process, 0, path.data(), &size)) {
            path.resize(size);
            dirs.push_back(fs::path(path).parent_path());
        }
        CloseHandle(process);
    }
    CloseHandle(snapshot);
    return dirs;
}

} // namespace

std::vector<fs::path> legacy_dirs(const LegacyHints& hints) {
    std::vector<fs::path> dirs{hints.working_dir};
    if (hints.old) dirs.push_back(hints.old->path.parent_path());
    dirs.push_back(hints.self_dir);
    for (const fs::path& dir : portable_shortcut_dirs()) dirs.push_back(dir);
    for (const fs::path& dir : portable_process_dirs()) dirs.push_back(dir);
    std::vector<fs::path> unique;
    for (const fs::path& dir : dirs) {
        const bool seen = std::ranges::any_of(unique, [&](const fs::path& u) {
            return _wcsicmp(u.lexically_normal().c_str(), dir.lexically_normal().c_str()) == 0;
        });
        if (dir.empty() || seen) continue;
        unique.push_back(dir);
        log("looking for portable data in {}", utf8(dir));
    }
    return unique;
}

bool wait_for_portable_exit(const std::wstring& object_prefix, std::chrono::milliseconds timeout) {
    const std::wstring name = L"Local\\" + object_prefix + L".Client.Instance";
    HANDLE mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name.c_str());
    if (!mutex) return true;
    log("waiting for the portable Parties to exit");
    const DWORD r = WaitForSingleObject(mutex, static_cast<DWORD>(timeout.count()));
    const bool released = r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
    if (released) ReleaseMutex(mutex);
    CloseHandle(mutex);
    if (released) log("the portable Parties has exited");
    else log("the portable Parties is still running");
    return released;
}

LegacyData move_legacy_data(const DataPaths& data, std::span<const fs::path> dirs) {
    const client::DataLocation location{data.data(), data.backups()};
    std::error_code ec;
    fs::create_directories(location.data_dir, ec);
    const client::LegacyImportResult imported = client::import_legacy_data(location, dirs);
    if (imported.status == client::LegacyImportResult::Status::Failed) return {imported.error, {}};
    if (imported.status == client::LegacyImportResult::Status::Imported)
        log("imported {} into {}", utf8(imported.imported_from), utf8(location.db_path()));
    const client::LegacyImportResult kept = client::preserve_legacy_data(location, dirs);
    if (kept.status == client::LegacyImportResult::Status::Failed) return {kept.error, {}};
    for (const auto* result : {&imported, &kept})
        for (const fs::path& copy : result->preserved) log("kept a legacy database as {}", utf8(copy));

    LegacyData out;
    const bool saved = !imported.preserved.empty() || !kept.preserved.empty();
    if (saved && imported.status != client::LegacyImportResult::Status::Imported)
        out.note = "A copy of your older Parties data was saved in " + utf8(data.backups()) + ".";
    return out;
}

void retire_old_binary(const OldBinary& old, const fs::path& self) {
    std::error_code ec;
    if (!fs::exists(old.path, ec) || fs::equivalent(old.path, self, ec)) return;
    const std::wstring name = old.path.filename().wstring();
    const bool replace = old.kind == OldBinary::Kind::Replace;
    if (replace ? !same_name(name, L"parties_client.exe")
                : !same_name(name, L"parties_client.old.exe") && !same_name(name, L"parties_client_new.exe"))
        return;
    // The old process has just exited, and Windows may keep its image locked a little longer.
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (replace ? CopyFileW(self.c_str(), old.path.c_str(), FALSE) : DeleteFileW(old.path.c_str())) {
            log("{} {}", replace ? "put the carrier in place of" : "deleted", utf8(old.path));
            return;
        }
        Sleep(250);
    }
    log("cannot retire {}: {}", utf8(old.path), last_error_text());
}

} // namespace parties::update::launcher
