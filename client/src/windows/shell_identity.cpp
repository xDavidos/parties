#include "shell_identity.h"

#include <parties/update/install_layout.h>

#include <propkey.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <string>

#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "shell32.lib")

namespace {

IPropertyStore* window_store(HWND hwnd) {
    IPropertyStore* store = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store)))) return nullptr;
    return store;
}

void set_string(IPropertyStore* store, const PROPERTYKEY& key, const std::wstring& value) {
    PROPVARIANT v;
    if (FAILED(InitPropVariantFromString(value.c_str(), &v))) return;
    store->SetValue(key, v);
    PropVariantClear(&v);
}

} // namespace

void apply_shell_identity(HWND hwnd) {
    wchar_t exe[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, exe, 32768);
    if (length == 0 || length >= 32768) return;
    auto root = parties::update::install_root_of(exe);
    if (!root) return;
    IPropertyStore* store = window_store(hwnd);
    if (!store) return;
    const std::wstring stub = (*root / parties::update::kStubFile).wstring();
    set_string(store, PKEY_AppUserModel_RelaunchCommand, L"\"" + stub + L"\"");
    set_string(store, PKEY_AppUserModel_RelaunchDisplayNameResource, L"Parties");
    set_string(store, PKEY_AppUserModel_RelaunchIconResource, stub + L",0");
    // Last, because the shell reads the relaunch properties when the ID changes.
    set_string(store, PKEY_AppUserModel_ID, parties::update::kAppUserModelId);
    store->Release();
}

void clear_shell_identity(HWND hwnd) {
    IPropertyStore* store = window_store(hwnd);
    if (!store) return;
    PROPVARIANT empty;
    PropVariantInit(&empty);
    for (const PROPERTYKEY& key : {PKEY_AppUserModel_ID, PKEY_AppUserModel_RelaunchCommand,
                                   PKEY_AppUserModel_RelaunchDisplayNameResource,
                                   PKEY_AppUserModel_RelaunchIconResource})
        store->SetValue(key, empty);
    store->Release();
}
