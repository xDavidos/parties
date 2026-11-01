// parties_manifest_tool: builds, signs and checks the signed channel manifests
// the release workflows publish. docs/releasing.md documents every command.

#include "release_ops.h"

#include <parties/crypto.h>
#include <parties/update/build_info.h>
#include <parties/update/manifest.h>
#include <parties/update/release_keys.h>

#include <windows.h>
#include <winhttp.h>

#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace parties::update;
using release::Result;

namespace {

constexpr const char* kUsage = R"(usage: parties_manifest_tool <command> [options]

  keygen  --out-secret <file> --out-public <file>
  entry   --build-info <json> --file <name>=<path>... --url-base <https url/>
          [--expect-version <A.B.C.D>] [--out <file>]
  merge   --base <envelope|none> --entry <json> --channel <stable|beta>
          --serial <n|now> --expires-days <d> --keep <n> --out <payload>
  promote --from <beta envelope> --to <stable envelope|none> --version <v>
          --serial <n|now> --expires-days <d> --keep <n> --out <payload>
  revoke  --beta <envelope|none> --stable <envelope|none> --version <v>
          --serial <n|now> --expires-days <d> --out-dir <dir>
  resign  --base <envelope> --channel <c> --serial <n|now> --expires-days <d> --out <payload>
  sign    --payload <file> --key-env <VAR>... --out <envelope>
  verify  --envelope <file> --channel <c> [--check-assets]
  pinned-keys
)"
#ifdef PARTIES_UPDATE_TESTING
R"(
Testing builds only: --keyring <dir> replaces the pinned production keys with
the *.pub files in <dir> for every command that verifies a manifest.
)"
#endif
;

struct UsageError {
    std::string message;
};

class Args {
public:
    Args(int argc, char** argv, std::set<std::string> allowed, std::set<std::string> switches)
        : switches_(std::move(switches)) {
#ifdef PARTIES_UPDATE_TESTING
        allowed.insert("--keyring");
#endif
        for (int i = 2; i < argc; ++i) {
            std::string flag = argv[i];
            if (switches_.contains(flag)) {
                values_[flag].push_back("");
                continue;
            }
            if (!allowed.contains(flag)) throw UsageError{"unknown option " + flag};
            if (i + 1 >= argc) throw UsageError{flag + " needs a value"};
            values_[flag].push_back(argv[++i]);
        }
    }

    bool has(const std::string& flag) const { return values_.contains(flag); }

    const std::string& one(const std::string& flag) const {
        auto it = values_.find(flag);
        if (it == values_.end()) throw UsageError{flag + " is required"};
        if (it->second.size() != 1) throw UsageError{flag + " is given more than once"};
        return it->second.front();
    }

    std::vector<std::string> all(const std::string& flag) const {
        auto it = values_.find(flag);
        if (it == values_.end()) throw UsageError{flag + " is required"};
        return it->second;
    }

    uint64_t number(const std::string& flag) const {
        const std::string& text = one(flag);
        uint64_t value = 0;
        auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc{} || end != text.data() + text.size()) throw UsageError{flag + " must be a number"};
        return value;
    }

private:
    std::set<std::string> switches_;
    std::map<std::string, std::vector<std::string>> values_;
};

struct Failure {
    std::string message;
};

template <class T>
T must(Result<T> r) {
    if (!r) throw Failure{r.error()};
    return std::move(*r);
}

std::string read_file(const fs::path& path, size_t limit = size_t(1) << 30) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Failure{"cannot read " + path.string()};
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() > limit) throw Failure{path.string() + " is too large"};
    return data;
}

void write_file(const fs::path& path, std::string_view data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), std::streamsize(data.size()));
    out.close();
    if (!out) throw Failure{"cannot write " + path.string()};
}

void write_new_file(const fs::path& path, std::string_view data) {
    if (fs::exists(path)) throw Failure{path.string() + " already exists; refusing to overwrite a key"};
    write_file(path, data);
}

std::span<const uint8_t> bytes_of(std::string_view s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

Sha256 sha256_of(std::string_view data) {
    std::string hex = parties::sha256_hex(bytes_of(data).data(), data.size());
    std::erase(hex, ':');
    return *Sha256::from_hex(hex);
}

std::string to_hex(std::span<const uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 15]);
    }
    return out;
}

template <size_t N>
std::optional<std::array<uint8_t, N>> from_hex(std::string_view hex) {
    while (!hex.empty() && std::isspace(static_cast<unsigned char>(hex.back()))) hex.remove_suffix(1);
    while (!hex.empty() && std::isspace(static_cast<unsigned char>(hex.front()))) hex.remove_prefix(1);
    if (hex.size() != 2 * N) return std::nullopt;
    std::array<uint8_t, N> out{};
    for (size_t i = 0; i < N; ++i) {
        auto [end, ec] = std::from_chars(hex.data() + 2 * i, hex.data() + 2 * i + 2, out[i], 16);
        if (ec != std::errc{} || end != hex.data() + 2 * i + 2) return std::nullopt;
    }
    return out;
}

// The key id is the first 16 hex characters of the public key, which is also
// what updater/keys/CMakeLists.txt derives for the pinned copy.
std::string key_id_of(const Ed25519PublicKey& pub) {
    return to_hex(pub).substr(0, 16);
}

std::vector<PinnedKey> load_keyring_dir(const fs::path& dir) {
    std::vector<PinnedKey> keys;
    for (const auto& item : fs::directory_iterator(dir)) {
        if (item.path().extension() != ".pub") continue;
        auto pub = from_hex<32>(read_file(item.path(), 4096));
        if (!pub) throw Failure{item.path().string() + " is not a 64 hex character public key"};
        keys.push_back({key_id_of(*pub), *pub});
    }
    return keys;
}

Keyring pinned_keyring(const Args& args) {
#ifdef PARTIES_UPDATE_TESTING
    if (args.has("--keyring")) {
        static std::vector<PinnedKey> test_keys = load_keyring_dir(args.one("--keyring"));
        return test_keys;
    }
#else
    (void)args;
#endif
    return production_keyring();
}

Channel channel_arg(const Args& args) {
    auto c = parse_channel(args.one("--channel"));
    if (!c) throw UsageError{"--channel must be stable or beta"};
    return *c;
}

Version version_arg(const Args& args) {
    auto v = Version::parse(args.one("--version"));
    if (!v) throw UsageError{"--version must be A.B.C.D"};
    return *v;
}

std::string_view kind_name(ManifestError::Kind kind) {
    using K = ManifestError::Kind;
    switch (kind) {
    case K::Malformed: return "malformed";
    case K::InvalidKeyring: return "no usable pinned keyring (is a production key committed?)";
    case K::UnknownKey: return "not signed by a pinned key";
    case K::BadSignature: return "bad signature";
    case K::WrongProduct: return "wrong product";
    case K::WrongChannel: return "wrong channel";
    case K::InvalidField: return "invalid field";
    }
    return "invalid";
}

VerifiedManifest verified_envelope(const Args& args, const fs::path& path, Channel channel) {
    std::string envelope = read_file(path, kMaxEnvelopeBytes + 1);
    auto m = verify_manifest(bytes_of(envelope), pinned_keyring(args), channel);
    if (!m)
        throw Failure{path.string() + " does not verify: " + std::string(kind_name(m.error().kind)) + ": " +
                      m.error().detail};
    return std::move(*m);
}

std::optional<VerifiedManifest> optional_envelope(const Args& args, const std::string& flag, Channel channel) {
    const std::string& value = args.one(flag);
    if (value == "none") return std::nullopt;
    return verified_envelope(args, value, channel);
}

Timestamp now() {
    return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
}

release::Issue issue_arg(const Args& args) {
    release::Issue issue;
    issue.issued_at = now();
    issue.serial = args.one("--serial") == "now" ? uint64_t(issue.issued_at.time_since_epoch().count())
                                                 : args.number("--serial");
    uint64_t days = args.number("--expires-days");
    if (days < 1 || days > 365) throw UsageError{"--expires-days must be between 1 and 365"};
    issue.lifetime = std::chrono::days(days);
    return issue;
}

size_t keep_arg(const Args& args) {
    uint64_t keep = args.number("--keep");
    if (keep < 1) throw UsageError{"--keep must be at least 1"};
    return size_t(keep);
}

void print_summary(const ManifestPayload& p) {
    std::cout << channel_name(p.channel) << " serial " << p.serial << ", " << p.builds.size() << " build(s)";
    for (const auto& b : p.builds) std::cout << " " << b.version.to_string();
    if (!p.revoked.empty()) {
        std::cout << ", revoked";
        for (const auto& v : p.revoked) std::cout << " " << v.to_string();
    }
    std::cout << "\n";
}

void emit_payload(const Args& args, const ManifestPayload& p) {
    write_file(args.one("--out"), write_payload(p));
    print_summary(p);
}

std::wstring widen(std::string_view s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

struct HttpHandle {
    HINTERNET h = nullptr;
    ~HttpHandle() {
        if (h) WinHttpCloseHandle(h);
    }
};

// Downloads at most `limit` bytes; one more means the asset is larger than the
// manifest says, which is enough to reject it.
Result<std::string> https_get(const std::string& url, uint64_t limit) {
    std::wstring wide = widen(url);
    URL_COMPONENTS parts{sizeof(parts)};
    wchar_t host[256]{}, path[4096]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = DWORD(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = DWORD(std::size(path));
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        return std::unexpected("not an https url: " + url);
    HttpHandle session{WinHttpOpen(L"parties-manifest-tool", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) return std::unexpected("WinHttpOpen failed");
    HttpHandle connection{WinHttpConnect(session.h, host, parts.nPort, 0)};
    if (!connection.h) return std::unexpected("cannot connect for " + url);
    HttpHandle request{WinHttpOpenRequest(connection.h, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!request.h || !WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
        return std::unexpected("request failed for " + url + " (error " + std::to_string(GetLastError()) + ")");
    DWORD status = 0, status_size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) return std::unexpected("HTTP " + std::to_string(status) + " for " + url);
    std::string body;
    for (;;) {
        char buf[64 * 1024];
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buf, sizeof(buf), &got)) return std::unexpected("read failed for " + url);
        if (got == 0) break;
        body.append(buf, got);
        if (body.size() > limit) break;
    }
    return body;
}

bool check_assets(const ManifestPayload& p) {
    bool ok = true;
    for (const auto& b : p.builds)
        for (const auto& f : b.files)
            for (const auto& url : f.urls) {
                auto body = https_get(url, f.size);
                if (!body) {
                    std::cerr << "asset check failed: " << body.error() << "\n";
                    ok = false;
                } else if (body->size() != f.size || sha256_of(*body) != f.sha256) {
                    std::cerr << "asset check failed: " << url << " does not match the manifest's size and sha256 for "
                              << b.version.to_string() << " " << f.name << "\n";
                    ok = false;
                } else {
                    std::cout << "asset ok: " << b.version.to_string() << " " << f.name << " " << url << "\n";
                }
            }
    return ok;
}

int cmd_keygen(const Args& args) {
    parties::SecretKey secret{};
    parties::PublicKey pub{};
    if (!parties::random_bytes(secret.data(), secret.size()) || !parties::derive_pubkey(secret, pub))
        throw Failure{"key generation failed"};
    write_new_file(args.one("--out-secret"), to_hex(secret) + "\n");
    write_new_file(args.one("--out-public"), to_hex(pub) + "\n");
    std::cout << "key id " << key_id_of(pub) << "\n";
    return 0;
}

int cmd_entry(const Args& args) {
    BuildInfo info = must(release::parse_build_info(read_file(args.one("--build-info"), 4096)));
    if (args.has("--expect-version") && args.one("--expect-version") != info.version)
        throw Failure{"the build reports version " + info.version + ", expected " + args.one("--expect-version")};
    std::vector<release::StagedFile> files;
    for (const std::string& spec : args.all("--file")) {
        auto eq = spec.find('=');
        if (eq == std::string::npos) throw UsageError{"--file takes <name>=<path>"};
        fs::path path = spec.substr(eq + 1);
        std::string data = read_file(path);
        files.push_back({spec.substr(0, eq), path.filename().string(), data.size(), sha256_of(data)});
    }
    BuildEntry entry = must(release::make_entry(info, files, args.one("--url-base")));
    std::string text = write_build_entry(entry);
    if (args.has("--out"))
        write_file(args.one("--out"), text);
    else
        std::cout << text << "\n";
    return 0;
}

int cmd_merge(const Args& args) {
    Channel channel = channel_arg(args);
    auto base = optional_envelope(args, "--base", channel);
    auto entry = parse_build_entry(read_file(args.one("--entry"), 1 << 20));
    if (!entry) throw Failure{"--entry: " + entry.error().detail};
    emit_payload(args, must(release::add_build(base ? &base->payload() : nullptr, channel, *entry, issue_arg(args),
                                               keep_arg(args))));
    return 0;
}

int cmd_promote(const Args& args) {
    VerifiedManifest beta = verified_envelope(args, args.one("--from"), Channel::Beta);
    auto stable = optional_envelope(args, "--to", Channel::Stable);
    emit_payload(args, must(release::promote(beta.payload(), stable ? &stable->payload() : nullptr, version_arg(args),
                                             issue_arg(args), keep_arg(args))));
    return 0;
}

// Every channel at once: setup installs follow Stable even when their build
// is only on Beta, so a revocation from one channel would miss them.
int cmd_revoke(const Args& args) {
    const Version version = version_arg(args);
    const release::Issue issue = issue_arg(args);
    std::vector<std::pair<Channel, ManifestPayload>> next;
    for (Channel channel : {Channel::Beta, Channel::Stable}) {
        auto base = optional_envelope(args, "--" + std::string(channel_name(channel)), channel);
        next.emplace_back(channel, must(release::revoke(base ? &base->payload() : nullptr, channel, version, issue)));
    }
    const fs::path dir = args.one("--out-dir");
    for (const auto& [channel, payload] : next) {
        write_file(dir / (std::string(channel_name(channel)) + ".payload.json"), write_payload(payload));
        print_summary(payload);
    }
    return 0;
}

int cmd_resign(const Args& args) {
    VerifiedManifest base = verified_envelope(args, args.one("--base"), channel_arg(args));
    emit_payload(args, must(release::reissue(base.payload(), issue_arg(args))));
    return 0;
}

int cmd_sign(const Args& args) {
    std::vector<SigningKey> keys;
    for (const std::string& var : args.all("--key-env")) {
        char value[256]{};
        DWORD length = GetEnvironmentVariableA(var.c_str(), value, DWORD(sizeof(value)));
        if (length == 0 || length >= sizeof(value)) throw Failure{"environment variable " + var + " is not set"};
        auto secret = from_hex<32>(std::string_view(value, length));
        SecureZeroMemory(value, sizeof(value));
        if (!secret) throw Failure{var + " is not a 64 hex character Ed25519 secret key"};
        SigningKey key;
        key.secret_key = *secret;
        if (!parties::derive_pubkey(key.secret_key, key.public_key)) throw Failure{"cannot derive the key in " + var};
        key.key_id = key_id_of(key.public_key);
        keys.push_back(std::move(key));
        std::cout << "signing with key id " << keys.back().key_id << "\n";
    }
    std::string payload = read_file(args.one("--payload"), kMaxEnvelopeBytes);
    auto envelope = sign_manifest(bytes_of(payload), keys);
    if (!envelope) throw Failure{"signing failed"};
    write_file(args.one("--out"), *envelope);
    return 0;
}

int cmd_verify(const Args& args) {
    VerifiedManifest m = verified_envelope(args, args.one("--envelope"), channel_arg(args));
    print_summary(m.payload());
    if (m.expires_at() <= now()) throw Failure{"the manifest has expired"};
    if (args.has("--check-assets") && !check_assets(m.payload())) throw Failure{"asset check failed"};
    std::cout << "verified\n";
    return 0;
}

int cmd_pinned_keys(const Args& args) {
    for (const PinnedKey& k : pinned_keyring(args)) std::cout << k.key_id << "\n";
    return 0;
}

int run(int argc, char** argv) {
    if (argc < 2) throw UsageError{"missing command"};
    std::string command = argv[1];
    const std::set<std::string> issue_flags{"--serial", "--expires-days", "--out"};
    auto with = [](std::set<std::string> a, const std::set<std::string>& b) {
        a.insert(b.begin(), b.end());
        return a;
    };
    if (command == "keygen") return cmd_keygen(Args(argc, argv, {"--out-secret", "--out-public"}, {}));
    if (command == "entry")
        return cmd_entry(Args(argc, argv, {"--build-info", "--file", "--url-base", "--expect-version", "--out"}, {}));
    if (command == "merge")
        return cmd_merge(Args(argc, argv, with({"--base", "--entry", "--channel", "--keep"}, issue_flags), {}));
    if (command == "promote")
        return cmd_promote(Args(argc, argv, with({"--from", "--to", "--version", "--keep"}, issue_flags), {}));
    if (command == "revoke")
        return cmd_revoke(Args(argc, argv, {"--beta", "--stable", "--version", "--serial", "--expires-days", "--out-dir"}, {}));
    if (command == "resign") return cmd_resign(Args(argc, argv, with({"--base", "--channel"}, issue_flags), {}));
    if (command == "sign") return cmd_sign(Args(argc, argv, {"--payload", "--key-env", "--out"}, {}));
    if (command == "verify") return cmd_verify(Args(argc, argv, {"--envelope", "--channel"}, {"--check-assets"}));
    if (command == "pinned-keys") return cmd_pinned_keys(Args(argc, argv, {}, {}));
    throw UsageError{"unknown command " + command};
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const UsageError& e) {
        std::cerr << "parties_manifest_tool: " << e.message << "\n\n" << kUsage;
        return 2;
    } catch (const Failure& e) {
        std::cerr << "parties_manifest_tool: " << e.message << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "parties_manifest_tool: " << e.what() << "\n";
        return 1;
    }
}
