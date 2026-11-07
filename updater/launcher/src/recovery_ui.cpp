#include "recovery_ui.h"

#include "file_io.h"
#include "log.h"
#include "recovery_choice.h"
#include "testing.h"

#include <algorithm>
#include <format>
#include <string>

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace parties::update::launcher {

namespace {

constexpr int kStartAgain = 100;
constexpr int kStartPrevious = 101;
constexpr int kRestore = 102;
constexpr int kOpenFolder = 103;
constexpr int kRetryBase = 200;

std::wstring version_text(const BuildId& id) { return widen(id.version.to_string()); }

std::wstring explanation(const std::optional<RecoveryReason>& why) {
    if (!why) return L"Choose how to start Parties.";
    switch (*why) {
    case RecoveryReason::GoodFailedToStart: return L"Parties did not start correctly.";
    case RecoveryReason::DataUnavailable:
        return L"Parties cannot open your data. Check that the data folder is reachable and not full, then try again.";
    case RecoveryReason::DataIncompatible:
        return L"Your data was saved by a newer version of Parties than the one installed.";
    case RecoveryReason::Repairing:
        return L"Some Parties files are damaged or missing. Start another version, or reinstall Parties.";
    }
    return {};
}

std::wstring backup_time(const Backup& b) {
    auto when = std::chrono::clock_cast<std::chrono::system_clock>(b.written);
    return widen(std::format("{:%Y-%m-%d %H:%M} UTC", std::chrono::floor<std::chrono::minutes>(when)));
}

HRESULT CALLBACK on_event(HWND, UINT notification, WPARAM, LPARAM, LONG_PTR data) {
    if (notification == TDN_CREATED) {
        auto* on_shown = reinterpret_cast<const std::function<void()>*>(data);
        if (*on_shown) (*on_shown)();
    }
    return S_OK;
}

std::optional<Snapshot> choose_backup(const RecoveryRequest& r) {
    std::vector<std::wstring> labels;
    std::vector<TASKDIALOG_BUTTON> radios;
    for (const Backup& b : r.backups)
        labels.push_back(backup_time(b) + widen(std::format("  {}  (data version {})", b.id.stem, b.data.version)));
    for (size_t i = 0; i < labels.size(); ++i) radios.push_back({static_cast<int>(1000 + i), labels[i].c_str()});
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof config;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    config.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
    config.pszWindowTitle = L"Parties Recovery";
    config.pszMainInstruction = L"Restore a backup";
    config.pszContent = L"Your current data is kept as a separate backup before the restore, so nothing is lost.";
    config.cRadioButtons = static_cast<UINT>(radios.size());
    config.pRadioButtons = radios.data();
    int button = 0, radio = 0;
    if (FAILED(TaskDialogIndirect(&config, &button, &radio, nullptr)) || button != IDOK) return std::nullopt;
    size_t index = static_cast<size_t>(radio - 1000);
    if (index >= r.backups.size()) return std::nullopt;
    return Snapshot{r.backups[index].id, r.backups[index].data};
}

} // namespace

RecoveryChoice show_recovery(const RecoveryRequest& r) {
    log("recovery shown: {}", recovery_reason_name(r.why));
    if (auto answer = testing::scripted_recovery()) {
        if (r.on_shown) r.on_shown();
        RecoveryChoice choice = scripted_choice(r.state, r.live_db, r.backups, *answer);
        log("recovery answered by script: {}", *answer);
        return choice;
    }

    const std::optional<StartBuild> previous = start_previous(r.state, r.live_db, r.backups);
    const std::vector<BuildId> retry = retryable(r.state);
    std::vector<std::wstring> labels;
    std::vector<TASKDIALOG_BUTTON> buttons;
    labels.reserve(8 + retry.size());
    auto add = [&](int id, std::wstring label) {
        labels.push_back(std::move(label));
        buttons.push_back({id, labels.back().c_str()});
    };
    add(kStartAgain, L"Start Parties " + version_text(r.state.good) + L"\nTry again with the current version.");
    if (previous && previous->build != r.state.good) {
        std::wstring label = L"Start the previous version (" + version_text(previous->build) + L")";
        if (previous->restore) {
            auto backup = std::ranges::find(r.backups, previous->restore->id, &Backup::id);
            label += L"\nStarts on your data from " + backup_time(*backup) +
                     L". Your current data is kept as a separate backup.";
        }
        add(kStartPrevious, std::move(label));
    }
    for (size_t i = 0; i < retry.size(); ++i)
        add(kRetryBase + static_cast<int>(i), L"Try Parties " + version_text(retry[i]) + L" again");
    if (restorable(r.state) && !r.backups.empty())
        add(kRestore, L"Restore a backup\nYour current data is kept as a separate backup.");
    add(kOpenFolder, L"Open the data folder");

    const std::wstring content = explanation(r.why);
    for (;;) {
        TASKDIALOGCONFIG config{};
        config.cbSize = sizeof config;
        config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
        config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
        config.pszWindowTitle = L"Parties Recovery";
        config.pszMainIcon = TD_WARNING_ICON;
        config.pszMainInstruction = L"Parties needs your help to start";
        config.pszContent = content.c_str();
        config.cButtons = static_cast<UINT>(buttons.size());
        config.pButtons = buttons.data();
        config.pfCallback = on_event;
        config.lpCallbackData = reinterpret_cast<LONG_PTR>(&r.on_shown);
        int button = 0;
        if (FAILED(TaskDialogIndirect(&config, &button, nullptr, nullptr))) {
            log("cannot show the recovery dialog");
            return Quit{};
        }
        if (button == kStartAgain) return StartAgain{};
        if (button == kStartPrevious && previous) return StartBuild{*previous};
        if (button >= kRetryBase && button < kRetryBase + static_cast<int>(retry.size()))
            return RetryBuild{retry[static_cast<size_t>(button - kRetryBase)]};
        if (button == kRestore) {
            if (auto snapshot = choose_backup(r)) return RestoreBackup{*snapshot};
            continue;
        }
        if (button == kOpenFolder) {
            ShellExecuteW(nullptr, L"open", r.data_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            continue;
        }
        return Quit{};
    }
}

} // namespace parties::update::launcher
