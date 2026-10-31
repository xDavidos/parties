// Stands in for PartiesLauncher.exe and parties_client.exe in update_stub_test.
// behavior.txt next to the exe holds "<behavior> <launcher sha256 hex>":
//   ok     write an unclean session report, signal boot_ok, then stay alive
//   clean  write a session report, signal boot_ok, mark the session clean, exit 0
//   fail   exit 1 without signalling
//   hang   never signal and never exit
//   app    exit 0 (a safe mode app)
// Every run first appends "<pid>|<exe name>|<arg1>|<arg2>..." to runs.log next to the exe.

#include <parties/update/boot.h>

#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
namespace boot = parties::update::boot;

namespace {

std::string narrow(std::wstring_view w) {
    std::string s(w.size() * 3, '\0');
    s.resize(WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), int(s.size()), nullptr, nullptr));
    return s;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    std::wstring self(32768, L'\0');
    self.resize(GetModuleFileNameW(nullptr, self.data(), DWORD(self.size())));
    fs::path dir = fs::path(self).parent_path();
    std::string behavior, launcher_hex;
    std::ifstream(dir / "behavior.txt") >> behavior >> launcher_hex;

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::string line = std::to_string(GetCurrentProcessId()) + "|" + narrow(fs::path(self).filename().wstring());
    fs::path root;
    uint64_t session = 0;
    HANDLE boot_ok = nullptr;
    for (int i = 1; i < argc; ++i) {
        std::wstring_view a = argv[i];
        line += "|" + narrow(a);
        if (a.starts_with(L"--install-root=")) root = a.substr(15);
        if (a.starts_with(L"--boot-session=")) session = std::stoull(std::wstring(a.substr(15)));
        if (a.starts_with(L"--boot-ok-event="))
            boot_ok = reinterpret_cast<HANDLE>(uintptr_t(std::stoull(std::wstring(a.substr(16)))));
    }
    std::ofstream(dir / "runs.log", std::ios::app | std::ios::binary) << line << "\n";

    if (behavior == "fail") return 1;
    if (behavior == "app") return 0;
    if (behavior == "hang") Sleep(INFINITE);

    auto launcher = parties::update::Sha256::from_hex(launcher_hex).value_or(parties::update::Sha256{});
    auto report = [&](bool clean) {
        std::ofstream(root / "state" / "launcher-session.json", std::ios::binary)
            << boot::write_session_report({session, launcher, clean});
    };
    report(false);
    SetEvent(boot_ok);
    if (behavior == "clean") {
        report(true);
        return 0;
    }
    Sleep(60'000);
    return 0;
}
