#include "shell_integration.h"

#include "file_io.h"
#include "log.h"
#include "testing.h"

#include <parties/update/install_layout.h>
#include <parties/update/manifest.h>

#include <fstream>
#include <variant>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "propsys.lib")

namespace parties::update::launcher {

namespace {

constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Parties";

template <class T>
struct Com {
    T* p = nullptr;
    ~Com() {
        if (p) p->Release();
    }
    T* operator->() const { return p; }
};

std::wstring uninstall_key() { return testing::uninstall_key().value_or(kUninstallKey); }

fs::path programs_folder() {
    if (auto folder = testing::start_menu()) return *folder;
    PWSTR path = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, KF_FLAG_CREATE, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

// Testing builds read a Desktop only when the harness names one.
std::optional<fs::path> desktop_folder() {
    if (auto folder = testing::desktop()) return *folder;
    if (testing::enabled()) return std::nullopt;
    PWSTR path = nullptr;
    std::optional<fs::path> out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

// Where the taskbar keeps pins. Testing builds read it only when the harness names one.
std::optional<fs::path> taskbar_pins_folder() {
    if (auto folder = testing::taskbar()) return *folder;
    if (testing::enabled()) return std::nullopt;
    PWSTR path = nullptr;
    std::optional<fs::path> out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path)))
        out = fs::path(path) / L"Microsoft" / L"Internet Explorer" / L"Quick Launch" / L"User Pinned" / L"TaskBar";
    CoTaskMemFree(path);
    return out;
}

fs::path expand(const std::wstring& text) {
    wchar_t expanded[32768]{};
    DWORD n = ExpandEnvironmentStringsW(text.c_str(), expanded, 32768);
    return n > 0 && n <= 32768 ? fs::path(std::wstring(expanded)) : fs::path(text);
}

std::wstring widen(std::string_view s) { return fs::path(std::u8string(s.begin(), s.end())).wstring(); }

// A shortcut reads back the long spelling of a target saved under its 8.3 name.
bool same_path(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return fs::equivalent(a, b, ec) || _wcsicmp(a.lexically_normal().c_str(), b.lexically_normal().c_str()) == 0;
}

struct RegValue {
    const wchar_t* name;
    std::variant<std::wstring, DWORD> value;
};

std::vector<RegValue> entry_values(const InstallPaths& install, const BuildId& good) {
    const std::wstring stub = (install.root / kStubFile).wstring();
    const std::wstring uninstall = uninstall_command(install, good);
    return {
        {L"DisplayName", L"Parties"},
        {L"DisplayVersion", widen(good.version.to_string())},
        {L"Publisher", L"Parties"},
        {L"InstallLocation", install.root.wstring()},
        {L"DisplayIcon", stub + L",0"},
        {L"UninstallString", uninstall},
        {L"QuietUninstallString", uninstall + L" --silent"},
        {L"NoModify", DWORD{1}},
        {L"NoRepair", DWORD{1}},
    };
}

bool set_values(HKEY key, const std::vector<RegValue>& values, std::string& error) {
    for (const RegValue& v : values) {
        LSTATUS status;
        if (auto* text = std::get_if<std::wstring>(&v.value))
            status = RegSetValueExW(key, v.name, 0, REG_SZ, reinterpret_cast<const BYTE*>(text->c_str()),
                                    static_cast<DWORD>((text->size() + 1) * sizeof(wchar_t)));
        else
            status = RegSetValueExW(key, v.name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&std::get<DWORD>(v.value)),
                                    sizeof(DWORD));
        if (status != ERROR_SUCCESS) {
            error = "cannot write the uninstall entry: error " + std::to_string(status);
            return false;
        }
    }
    return true;
}

// The entry's key, opened only if it describes this install root.
HKEY open_own_entry(const InstallPaths& install, REGSAM access) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, uninstall_key().c_str(), 0, access | KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return nullptr;
    wchar_t location[32768]{};
    DWORD size = sizeof(location) - sizeof(wchar_t);
    if (RegGetValueW(key, nullptr, L"InstallLocation", RRF_RT_REG_SZ, nullptr, location, &size) != ERROR_SUCCESS ||
        !same_path(location, install.root)) {
        RegCloseKey(key);
        return nullptr;
    }
    return key;
}

struct LinkInfo {
    std::wstring target, arguments, aumid, working_dir;
};

std::optional<LinkInfo> read_link(const fs::path& file) {
    Com<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link.p))))
        return std::nullopt;
    Com<IPersistFile> persist;
    if (FAILED(link->QueryInterface(IID_PPV_ARGS(&persist.p))) || FAILED(persist->Load(file.c_str(), STGM_READ)))
        return std::nullopt;
    LinkInfo info;
    wchar_t buffer[32768]{};
    if (SUCCEEDED(link->GetPath(buffer, 32768, nullptr, SLGP_RAWPATH))) info.target = buffer;
    if (SUCCEEDED(link->GetArguments(buffer, 32768))) info.arguments = buffer;
    if (SUCCEEDED(link->GetWorkingDirectory(buffer, 32768))) info.working_dir = buffer;
    Com<IPropertyStore> props;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&props.p)))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(PKEY_AppUserModel_ID, &v)) && v.vt == VT_LPWSTR) info.aumid = v.pwszVal;
        PropVariantClear(&v);
    }
    return info;
}

bool save_link(const Shortcut& s, std::string& error) {
    Com<IShellLinkW> link;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link.p));
    Com<IPropertyStore> props;
    Com<IPersistFile> persist;
    PROPVARIANT id;
    PropVariantInit(&id);
    if (SUCCEEDED(hr)) hr = link->SetPath(s.target.c_str());
    if (SUCCEEDED(hr)) hr = link->SetArguments(s.arguments.c_str());
    if (SUCCEEDED(hr)) hr = link->SetDescription(s.description.c_str());
    if (SUCCEEDED(hr)) hr = link->SetWorkingDirectory(s.target.parent_path().c_str());
    if (SUCCEEDED(hr)) hr = link->QueryInterface(IID_PPV_ARGS(&props.p));
    if (SUCCEEDED(hr)) hr = InitPropVariantFromString(kAppUserModelId, &id);
    if (SUCCEEDED(hr)) hr = props->SetValue(PKEY_AppUserModel_ID, id);
    PropVariantClear(&id);
    if (SUCCEEDED(hr)) hr = props->Commit();
    if (SUCCEEDED(hr)) hr = link->QueryInterface(IID_PPV_ARGS(&persist.p));
    fs::path tmp = s.file;
    tmp += L".tmp";
    if (SUCCEEDED(hr)) hr = persist->Save(tmp.c_str(), TRUE);
    if (FAILED(hr)) {
        error = "cannot create the shortcut " + utf8(s.file) + ": error " + std::to_string(hr);
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), s.file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "cannot create the shortcut " + utf8(s.file) + ": " + last_error_text();
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// Refuses every item the shell is about to delete instead of recycle.
class RecycleOnly final : public IFileOperationProgressSink {
public:
    bool refused = false;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IUnknown || iid == __uuidof(IFileOperationProgressSink)) {
            *out = static_cast<IFileOperationProgressSink*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD flags, IShellItem*) override {
        if (flags & TSF_DELETE_RECYCLE_IF_POSSIBLE) return S_OK;
        refused = true;
        return E_ABORT;
    }

    HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem*, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }
};

} // namespace

std::vector<Shortcut> shortcuts(const InstallPaths& install) {
    const fs::path folder = programs_folder();
    const fs::path stub = install.root / kStubFile;
    return {
        {folder / L"Parties.lnk", stub, L"", L"Start Parties"},
        {folder / L"Parties Recovery.lnk", stub, L"--recover",
         L"Start an earlier version of Parties, restore a backup or repair the install"},
    };
}

bool write_shortcuts(const InstallPaths& install, std::string& error) {
    for (const Shortcut& s : shortcuts(install)) {
        std::error_code ec;
        fs::create_directories(s.file.parent_path(), ec);
        auto existing = read_link(s.file);
        if (existing && same_path(existing->target, s.target) && existing->arguments == s.arguments &&
            existing->aumid == kAppUserModelId)
            continue;
        if (!save_link(s, error)) return false;
        log("wrote shortcut {}", utf8(s.file));
    }
    return true;
}

std::vector<fs::path> portable_shortcut_dirs() {
    std::vector<fs::path> links;
    std::error_code ec;
    auto collect = [&](const fs::path& folder, bool recursive) {
        if (folder.empty()) return;
        auto add = [&](const fs::directory_entry& e) {
            if (e.is_regular_file(ec) && _wcsicmp(e.path().extension().c_str(), L".lnk") == 0) links.push_back(e.path());
        };
        constexpr auto options = fs::directory_options::skip_permission_denied;
        if (recursive)
            for (const auto& e : fs::recursive_directory_iterator(folder, options, ec)) add(e);
        else
            for (const auto& e : fs::directory_iterator(folder, options, ec)) add(e);
    };
    collect(programs_folder(), true);
    if (auto desktop = desktop_folder()) collect(*desktop, false);
    if (auto pins = taskbar_pins_folder()) collect(*pins, false);

    std::vector<fs::path> dirs;
    for (const fs::path& file : links) {
        auto link = read_link(file);
        if (!link || link->target.empty()) continue;
        const fs::path target = expand(link->target);
        const bool portable = _wcsicmp(target.filename().c_str(), L"parties_client.exe") == 0 ||
                              _wcsicmp(file.filename().c_str(), L"Parties.lnk") == 0;
        if (!portable) continue;
        dirs.push_back(target.parent_path());
        if (!link->working_dir.empty()) dirs.push_back(expand(link->working_dir));
    }
    return dirs;
}

void remove_shortcuts(const InstallPaths& install) {
    for (const Shortcut& s : shortcuts(install)) {
        if (DeleteFileW(s.file.c_str())) log("removed shortcut {}", utf8(s.file));
    }
}

std::wstring uninstall_command(const InstallPaths& install, const BuildId& good) {
    const fs::path launcher = install.builds() / good.dir_name() / kLauncherFile;
    return L"\"" + launcher.wstring() + L"\" --uninstall \"--install-root=" + install.root.wstring() + L"\"";
}

bool write_uninstall_entry(const InstallPaths& install, const BuildId& good, std::string& error) {
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, uninstall_key().c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                                     nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        error = "cannot create the uninstall entry: error " + std::to_string(status);
        return false;
    }
    bool ok = set_values(key, entry_values(install, good), error);
    RegCloseKey(key);
    return ok;
}

void refresh_uninstall_entry(const InstallPaths& install, const BuildId& good) {
    HKEY key = open_own_entry(install, KEY_SET_VALUE);
    if (!key) return;
    std::string error;
    if (!set_values(key, entry_values(install, good), error)) log("{}", error);
    RegCloseKey(key);
}

void remove_uninstall_entry(const InstallPaths& install) {
    HKEY key = open_own_entry(install, 0);
    if (!key) return;
    RegCloseKey(key);
    LSTATUS status = RegDeleteKeyW(HKEY_CURRENT_USER, uninstall_key().c_str());
    if (status == ERROR_SUCCESS) log("removed the uninstall entry");
    else log("cannot remove the uninstall entry: error {}", status);
}

bool recycle(const fs::path& path, std::string& error) {
    if (auto record = testing::recycle_log()) {
        std::ofstream(*record, std::ios::app | std::ios::binary) << utf8(path) << "\n";
        return true;
    }
    Com<IShellItem> item;
    Com<IFileOperation> op;
    RecycleOnly sink;
    DWORD cookie = 0;
    HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item.p));
    if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op.p));
    if (SUCCEEDED(hr)) hr = op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NO_UI | FOFX_RECYCLEONDELETE);
    if (SUCCEEDED(hr)) hr = op->Advise(&sink, &cookie);
    if (SUCCEEDED(hr)) hr = op->DeleteItem(item.p, nullptr);
    if (SUCCEEDED(hr)) hr = op->PerformOperations();
    BOOL aborted = FALSE;
    if (op.p) op->GetAnyOperationsAborted(&aborted);
    if (FAILED(hr) || aborted || sink.refused) {
        error = sink.refused ? utf8(path) + " cannot go to the Recycle Bin"
                             : "cannot move " + utf8(path) + " to the Recycle Bin: error " + std::to_string(hr);
        return false;
    }
    log("moved {} to the Recycle Bin", utf8(path));
    return true;
}

} // namespace parties::update::launcher
