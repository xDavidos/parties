#include "file_io.h"
#include "setup.h"
#include "supervisor.h"

#include <string>
#include <vector>

#include <windows.h>
#include <shellapi.h>

using namespace parties::update::launcher;

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    LocalFree(argv);

    if (auto code = run_setup_modes(args)) return *code;
    std::string error;
    auto options = parse_options(args, error);
    if (!options) {
        std::wstring text(error.begin(), error.end());
        MessageBoxW(nullptr, (L"Parties cannot start: " + text).c_str(), L"Parties", MB_OK | MB_ICONERROR);
        return 1;
    }
    return run_launcher(*options);
}
