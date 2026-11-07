// Writes a macOS build's entry, the seed's Contents/Resources/seed/build.json,
// with the codec the launcher parses it with: this tree's version and data
// schema, and the size and SHA-256 of both payload dylibs as they are now.
// Run it after the last change to either file, a signature included.
//
//   parties_build_json --launcher <dylib> --app <dylib> --out <file> [--url-base <https://.../>]

#include <client/settings_schema.h>
#include <parties/version_gen.h>
#include <parties/update/link_protocol.h>
#include <parties/update/manifest.h>
#include <parties/update/product.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>

namespace fs = std::filesystem;
using namespace parties::update;

namespace {

int fail(const std::string& why) {
    std::fprintf(stderr, "parties_build_json: %s\n", why.c_str());
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> args{{"--url-base", "https://github.com/emcifuntik/parties/releases/download/"
                                                           "v" PARTIES_VERSION_STR "/"}};
    for (int i = 1; i + 1 < argc; i += 2) args[argv[i]] = argv[i + 1];
    if (!args.contains("--launcher") || !args.contains("--app") || !args.contains("--out")) {
        std::fprintf(stderr, "usage: parties_build_json --launcher <dylib> --app <dylib> --out <file> [--url-base <url>]\n");
        return 2;
    }
    const auto version = Version::parse(PARTIES_VERSION_STR);
    if (!version) return fail("version " PARTIES_VERSION_STR " is not A.B.C.D");
    namespace schema = parties::client::schema;
    BuildEntry entry{.version = *version,
                     .data_schema = uint32_t(schema::kCurrent.version),
                     .data_compat_floor = uint32_t(schema::kCurrent.compat_floor),
                     .link_protocol = kLinkProtocol,
                     .min_supported_launcher_link = kMinLauncherLink};
    for (auto [name, flag] : {std::pair{kMacClient.launcher_file, "--launcher"}, std::pair{kMacClient.app_file, "--app"}}) {
        std::ifstream in(args[flag], std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!in && !in.eof()) return fail("cannot read " + args[flag]);
        if (bytes.empty()) return fail(args[flag] + " is empty");
        entry.files.push_back({std::string(name), bytes.size(),
                               sha256_of({reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()}),
                               {args["--url-base"] + std::string(name)}});
    }
    const std::string text = write_build_entry(entry);
    if (auto checked = parse_build_entry(text, kMacClient); !checked) return fail(checked.error().detail);
    const fs::path out = args["--out"];
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    std::ofstream file(out, std::ios::binary | std::ios::trunc);
    file << text;
    file.close();
    if (!file) return fail("cannot write " + out.string());
    std::printf("%s: %s %s\n", out.string().c_str(), version->to_string().c_str(), entry.id().dir_name().c_str());
    return 0;
}
