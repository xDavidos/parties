// Drives the real parties_manifest_tool.exe through a release's life in a
// temporary directory: keygen, entry, merge into an empty beta, sign, verify,
// promote, revoke, resign, and the refusals that keep channels monotonic and
// signed by pinned keys.
// Usage: update_manifest_tool_test <path to parties_manifest_tool.exe>

#include <parties/crypto.h>
#include <parties/update/build_info.h>
#include <parties/update/manifest.h>
#include <parties/update/release_keys.h>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace parties::update;

namespace {

fs::path g_tool;
fs::path g_dir;

bool fail(const std::string& msg) {
    std::cerr << "update_manifest_tool_test: " << msg << "\n";
    return false;
}

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write(const fs::path& p, std::string_view data) {
    std::ofstream(p, std::ios::binary).write(data.data(), std::streamsize(data.size()));
}

std::wstring quote(const std::wstring& arg) {
    std::wstring out = L"\"";
    for (wchar_t c : arg) {
        if (c == L'"') out += L'\\';
        out += c;
    }
    return out + L"\"";
}

struct Run {
    DWORD exit_code = 0xFFFFFFFF;
    std::string output;
};

Run run(std::initializer_list<std::string> args) {
    std::wstring cmd = quote(g_tool.wstring());
    for (const auto& a : args) cmd += L" " + quote(fs::path(a).wstring());
    fs::path out_path = g_dir / "tool-output.txt";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out = CreateFileW(out_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out;
    si.hStdError = out;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    Run r;
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, g_dir.c_str(), &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &r.exit_code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    CloseHandle(out);
    r.output = read(out_path);
    return r;
}

bool ok(const Run& r, const std::string& what) {
    if (r.exit_code == 0) return true;
    return fail(what + " failed (exit " + std::to_string(r.exit_code) + "): " + r.output);
}

bool refused(const Run& r, const std::string& what, std::string_view reason) {
    if (r.exit_code == 0) return fail(what + " must be refused");
    if (r.output.find(reason) == std::string::npos) return fail(what + " refused for the wrong reason: " + r.output);
    return true;
}

std::string sha256_hex(const std::string& data) {
    std::string hex = parties::sha256_hex(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    std::erase(hex, ':');
    return hex;
}

std::vector<PinnedKey> keyring_from(const fs::path& pub_file) {
    std::string hex = read(pub_file).substr(0, 64);
    PinnedKey key;
    key.key_id = hex.substr(0, 16);
    for (size_t i = 0; i < 32; ++i) key.public_key[i] = uint8_t(std::stoul(hex.substr(2 * i, 2), nullptr, 16));
    return {key};
}

std::optional<VerifiedManifest> load(const fs::path& envelope, const std::vector<PinnedKey>& ring, Channel channel) {
    std::string bytes = read(envelope);
    auto m = verify_manifest({reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()}, ring, channel);
    if (!m) return std::nullopt;
    return std::move(*m);
}

bool sign_and_verify(const std::string& payload, const std::string& envelope, const char* channel) {
    return ok(run({"sign", "--payload", payload, "--key-env", "PARTIES_TEST_UPDATE_KEY", "--out", envelope}), "sign " + payload) &&
           ok(run({"verify", "--envelope", envelope, "--channel", channel, "--keyring", "ring"}), "verify " + envelope);
}

bool test_release_life() {
    fs::create_directories(g_dir / "ring");
    fs::create_directories(g_dir / "other");
    if (!ok(run({"keygen", "--out-secret", "secret.hex", "--out-public", "ring/release.pub"}), "keygen") ||
        !ok(run({"keygen", "--out-secret", "other-secret.hex", "--out-public", "other/other.pub"}), "second keygen"))
        return false;
    if (!refused(run({"keygen", "--out-secret", "secret.hex", "--out-public", "ring/again.pub"}), "keygen over a key",
                 "refusing to overwrite"))
        return false;
    auto ring = keyring_from(g_dir / "ring/release.pub");
    SetEnvironmentVariableA("PARTIES_TEST_UPDATE_KEY", read(g_dir / "secret.hex").c_str());

    const std::string launcher = "launcher bytes", app = "app bytes, a little longer";
    write(g_dir / "PartiesLauncher-1.2.3.4.exe", launcher);
    write(g_dir / "parties_client-1.2.3.4.exe", app);
    write(g_dir / "build-info.json", format_build_info({"1.2.3.4", 2, 1, 1, 1}));
    const std::string url_base = "https://github.com/emcifuntik/parties/releases/download/v1.2.3.4/";
    if (!refused(run({"entry", "--build-info", "build-info.json", "--file", "PartiesLauncher.exe=PartiesLauncher-1.2.3.4.exe",
                      "--file", "parties_client.exe=parties_client-1.2.3.4.exe", "--url-base", url_base,
                      "--expect-version", "1.2.3.5", "--out", "entry.json"}),
                 "an entry whose build reports another version", "expected 1.2.3.5"))
        return false;
    if (!ok(run({"entry", "--build-info", "build-info.json", "--file", "PartiesLauncher.exe=PartiesLauncher-1.2.3.4.exe",
                 "--file", "parties_client.exe=parties_client-1.2.3.4.exe", "--url-base", url_base,
                 "--expect-version", "1.2.3.4", "--out", "entry.json"}),
            "entry"))
        return false;
    const std::string entry_bytes = read(g_dir / "entry.json");
    auto entry = parse_build_entry(entry_bytes);
    if (!entry || entry->version.to_string() != "1.2.3.4" || entry->data_schema != 2)
        return fail("entry carries the build info: " + entry_bytes);
    const FileEntry* app_file = entry->file(kAppFile);
    if (!app_file || app_file->size != app.size() || app_file->sha256.to_hex() != sha256_hex(app) ||
        app_file->urls != std::vector<std::string>{url_base + "parties_client-1.2.3.4.exe"})
        return fail("entry describes the app file and its release asset: " + entry_bytes);

    if (!ok(run({"merge", "--base", "none", "--entry", "entry.json", "--channel", "beta", "--serial", "100",
                 "--expires-days", "45", "--keep", "5", "--out", "beta.payload.json"}),
            "merge into an empty beta") ||
        !sign_and_verify("beta.payload.json", "beta.manifest.json", "beta"))
        return false;
    auto beta = load(g_dir / "beta.manifest.json", ring, Channel::Beta);
    if (!beta || beta->serial() != 100 || beta->builds().size() != 1 || beta->builds()[0] != *entry)
        return fail("the signed beta manifest holds the entry");

    if (!ok(run({"promote", "--from", "beta.manifest.json", "--to", "none", "--version", "1.2.3.4", "--serial", "100",
                 "--expires-days", "45", "--keep", "5", "--out", "stable.payload.json", "--keyring", "ring"}),
            "promote") ||
        !sign_and_verify("stable.payload.json", "stable.manifest.json", "stable"))
        return false;
    if (read(g_dir / "beta.payload.json").find(entry_bytes) == std::string::npos ||
        read(g_dir / "stable.payload.json").find(entry_bytes) == std::string::npos)
        return fail("the promoted entry must be the same bytes in beta and in stable");
    auto stable = load(g_dir / "stable.manifest.json", ring, Channel::Stable);
    if (!stable || stable->builds().size() != 1 || stable->builds()[0] != *entry) return fail("stable holds the beta entry");

    if (!ok(run({"revoke", "--base", "beta.manifest.json", "--channel", "beta", "--version", "1.2.3.4", "--serial", "101",
                 "--expires-days", "45", "--out", "beta-revoked.payload.json", "--keyring", "ring"}),
            "revoke") ||
        !sign_and_verify("beta-revoked.payload.json", "beta-revoked.manifest.json", "beta"))
        return false;
    auto revoked = load(g_dir / "beta-revoked.manifest.json", ring, Channel::Beta);
    if (!revoked || !revoked->builds().empty() || !revoked->is_revoked(entry->version) || revoked->serial() != 101)
        return fail("revoke drops the build and lists it as revoked");

    if (!ok(run({"resign", "--base", "stable.manifest.json", "--channel", "stable", "--serial", "now", "--expires-days",
                 "45", "--out", "stable-resigned.payload.json", "--keyring", "ring"}),
            "resign") ||
        !sign_and_verify("stable-resigned.payload.json", "stable-resigned.manifest.json", "stable"))
        return false;
    auto resigned = load(g_dir / "stable-resigned.manifest.json", ring, Channel::Stable);
    if (!resigned || resigned->serial() <= stable->serial() || !std::ranges::equal(resigned->builds(), stable->builds()) ||
        resigned->expires_at() < stable->expires_at())
        return fail("resign bumps the serial and keeps the entries");

    if (!refused(run({"merge", "--base", "beta.manifest.json", "--entry", "entry.json", "--channel", "beta",
                      "--serial", "50", "--expires-days", "45", "--keep", "5", "--out", "lower.payload.json", "--keyring",
                      "ring"}),
                 "a merge with a lower serial", "does not exceed the published serial 100"))
        return false;

    std::string tampered = read(g_dir / "stable.manifest.json");
    size_t at = tampered.find("\"payload\":\"") + 20;
    tampered[at] = tampered[at] == 'A' ? 'B' : 'A';
    write(g_dir / "tampered.manifest.json", tampered);
    if (!refused(run({"verify", "--envelope", "tampered.manifest.json", "--channel", "stable", "--keyring", "ring"}),
                 "a tampered envelope", "does not verify"))
        return false;

    SetEnvironmentVariableA("PARTIES_TEST_OTHER_KEY", read(g_dir / "other-secret.hex").c_str());
    if (!ok(run({"sign", "--payload", "beta.payload.json", "--key-env", "PARTIES_TEST_OTHER_KEY", "--out",
                 "foreign.manifest.json"}),
            "sign with a key that is not pinned"))
        return false;
    if (!refused(run({"merge", "--base", "foreign.manifest.json", "--entry", "entry.json", "--channel", "beta", "--serial",
                      "200", "--expires-days", "45", "--keep", "5", "--out", "foreign.payload.json", "--keyring", "ring"}),
                 "a merge onto a base no pinned key signed", "not signed by a pinned key"))
        return false;
    if (!refused(run({"promote", "--from", "foreign.manifest.json", "--to", "none", "--version", "1.2.3.4", "--serial",
                      "200", "--expires-days", "45", "--keep", "5", "--out", "foreign.payload.json", "--keyring", "ring"}),
                 "a promotion from a base no pinned key signed", "not signed by a pinned key"))
        return false;

    if (production_keyring().empty() &&
        !refused(run({"verify", "--envelope", "beta.manifest.json", "--channel", "beta"}),
                 "verify without production keys", "no usable pinned keyring"))
        return false;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return fail("usage: update_manifest_tool_test <parties_manifest_tool.exe>") ? 0 : 1;
    g_tool = fs::absolute(argv[1]);
    g_dir = fs::temp_directory_path() / ("parties_manifest_tool_test_" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(g_dir);
    fs::create_directories(g_dir);
    bool passed = test_release_life();
    if (passed) {
        fs::remove_all(g_dir);
        std::cout << "update_manifest_tool_test: all passed\n";
    } else {
        std::cerr << "update_manifest_tool_test: left " << g_dir.string() << " for inspection\n";
    }
    return passed ? 0 : 1;
}
