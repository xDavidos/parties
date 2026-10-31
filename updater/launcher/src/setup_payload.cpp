#include "setup_payload.h"

#include "file_io.h"
#include "log.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <fstream>
#include <iterator>

#include <windows.h>

namespace parties::update::launcher {

namespace {

constexpr wchar_t kManifestRes[] = L"PARTIES_MANIFEST";
constexpr wchar_t kIndexRes[] = L"PARTIES_INDEX";
constexpr wchar_t kStubRes[] = L"PARTIES_STUB";
constexpr WORD kLanguage = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);

std::wstring file_res(size_t i) { return L"PARTIES_FILE_" + std::to_wstring(i); }

using Bytes = std::span<const uint8_t>;

std::optional<Bytes> resource(const wchar_t* name) {
    HRSRC found = FindResourceW(nullptr, name, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!found) return std::nullopt;
    HGLOBAL loaded = LoadResource(nullptr, found);
    const void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data) return std::nullopt;
    return Bytes(static_cast<const uint8_t*>(data), SizeofResource(nullptr, found));
}

std::expected<VerifiedManifest, ManifestError> verify_any_channel(Bytes envelope, Keyring keys) {
    auto verified = verify_manifest(envelope, keys, Channel::Stable);
    if (!verified && verified.error().kind == ManifestError::Kind::WrongChannel)
        verified = verify_manifest(envelope, keys, Channel::Beta);
    return verified;
}

bool matches(Bytes bytes, uint64_t size, const Sha256& sha256) {
    auto digest = digest_bytes(bytes);
    return digest && digest->size == size && digest->sha256 == sha256;
}

struct Index {
    Version version;
    uint64_t stub_size = 0;
    Sha256 stub_sha256;
};

std::string write_index(const Index& index) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
    w.StartObject();
    w.Key("format"); w.Uint(1);
    w.Key("version"); w.String(index.version.to_string().c_str());
    w.Key("stub_size"); w.Uint64(index.stub_size);
    w.Key("stub_sha256"); w.String(index.stub_sha256.to_hex().c_str());
    w.EndObject();
    return {buffer.GetString(), buffer.GetSize()};
}

std::optional<Index> parse_index(Bytes bytes) {
    rapidjson::Document doc;
    doc.Parse(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (doc.HasParseError() || !doc.IsObject()) return std::nullopt;
    auto version = doc.FindMember("version");
    auto size = doc.FindMember("stub_size");
    auto sha = doc.FindMember("stub_sha256");
    if (version == doc.MemberEnd() || !version->value.IsString() || size == doc.MemberEnd() ||
        !size->value.IsUint64() || sha == doc.MemberEnd() || !sha->value.IsString())
        return std::nullopt;
    auto v = Version::parse(version->value.GetString());
    auto digest = Sha256::from_hex(sha->value.GetString());
    if (!v || !digest) return std::nullopt;
    return Index{*v, size->value.GetUint64(), *digest};
}

std::optional<std::vector<uint8_t>> read_all(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) return std::nullopt;
    return bytes;
}

class PayloadSource final : public UpdateSource {
public:
    explicit PayloadSource(const Payload& payload) : payload_(payload) {}

    std::expected<std::vector<uint8_t>, SourceError> fetch_manifest(Channel, const std::atomic<bool>&) override {
        return std::unexpected(SourceError{"setup carries no channel manifest to follow"});
    }

    std::expected<void, SourceError> fetch_file(const FileEntry& file, const fs::path& dest, const Progress& progress,
                                                const std::atomic<bool>&) override {
        for (size_t i = 0; i < payload_.entry.files.size(); ++i) {
            if (payload_.entry.files[i].name != file.name) continue;
            Bytes bytes = payload_.files[i];
            std::ofstream out(dest, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!out.flush()) return std::unexpected(SourceError{"cannot write " + utf8(dest)});
            if (progress) progress(bytes.size());
            return {};
        }
        return std::unexpected(SourceError{"setup does not carry " + file.name});
    }

private:
    const Payload& payload_;
};

} // namespace

std::optional<std::expected<Payload, std::string>> embedded_payload(Keyring keys) {
    auto manifest = resource(kManifestRes);
    if (!manifest) return std::nullopt;
    auto fail = [](std::string why) { return std::optional(std::expected<Payload, std::string>(std::unexpected(why))); };
    auto index_bytes = resource(kIndexRes);
    auto stub = resource(kStubRes);
    auto index = index_bytes ? parse_index(*index_bytes) : std::nullopt;
    if (!index || !stub) return fail("this setup file is damaged");
    auto verified = verify_any_channel(*manifest, keys);
    if (!verified) return fail("this setup file is not signed by a Parties release key (" + verified.error().detail + ")");
    const BuildEntry* entry = verified->find(index->version);
    if (!entry) return fail("this setup file is damaged: its manifest does not list " + index->version.to_string());
    if (!matches(*stub, index->stub_size, index->stub_sha256)) return fail("this setup file is damaged: Parties.exe");
    Payload payload{*entry, *stub, {}};
    for (size_t i = 0; i < entry->files.size(); ++i) {
        const FileEntry& f = entry->files[i];
        auto bytes = resource(file_res(i).c_str());
        if (!bytes || !matches(*bytes, f.size, f.sha256)) return fail("this setup file is damaged: " + f.name);
        payload.files.push_back(*bytes);
    }
    return std::expected<Payload, std::string>(std::move(payload));
}

bool pack_payload(const fs::path& dir, const fs::path& out, Keyring keys, std::string& error) {
    auto manifest = read_all(dir / kPayloadManifest);
    auto stub = read_all(dir / "Parties.exe");
    if (!manifest || !stub) {
        error = utf8(dir) + " needs " + std::string(kPayloadManifest) + " and Parties.exe";
        return false;
    }
    auto verified = verify_any_channel(*manifest, keys);
    if (!verified) {
        error = std::string(kPayloadManifest) + " does not verify against the pinned keys: " + verified.error().detail;
        return false;
    }
    const BuildEntry* chosen = nullptr;
    std::vector<std::vector<uint8_t>> files;
    for (const BuildEntry& entry : verified->builds()) {
        files.clear();
        for (const FileEntry& f : entry.files) {
            auto bytes = read_all(dir / f.name);
            if (!bytes || !matches(*bytes, f.size, f.sha256)) break;
            files.push_back(std::move(*bytes));
        }
        if (files.size() == entry.files.size()) {
            chosen = &entry;
            break;
        }
    }
    if (!chosen) {
        error = "no build in " + std::string(kPayloadManifest) + " matches the files in " + utf8(dir);
        return false;
    }
    auto stub_digest = digest_bytes(*stub);
    if (!stub_digest) {
        error = "cannot hash Parties.exe";
        return false;
    }
    const std::string index = write_index({chosen->version, stub_digest->size, stub_digest->sha256});

    std::wstring self(32768, L'\0');
    self.resize(GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size())));
    if (!CopyFileW(self.c_str(), out.c_str(), FALSE)) {
        error = "cannot write " + utf8(out) + ": " + last_error_text();
        return false;
    }
    HANDLE update = BeginUpdateResourceW(out.c_str(), FALSE);
    bool ok = update != nullptr;
    auto add = [&](const std::wstring& name, const void* data, size_t size) {
        ok = ok && UpdateResourceW(update, MAKEINTRESOURCEW(10), name.c_str(), kLanguage, const_cast<void*>(data),
                                   static_cast<DWORD>(size));
    };
    add(kManifestRes, manifest->data(), manifest->size());
    add(kIndexRes, index.data(), index.size());
    add(kStubRes, stub->data(), stub->size());
    for (size_t i = 0; i < files.size(); ++i) add(file_res(i), files[i].data(), files[i].size());
    if (update && !EndUpdateResourceW(update, !ok)) ok = false;
    if (!ok) {
        error = "cannot embed the payload in " + utf8(out) + ": " + last_error_text();
        DeleteFileW(out.c_str());
        return false;
    }
    log("packed {} {} into {}", kAppFile, chosen->version.to_string(), utf8(out));
    return true;
}

std::unique_ptr<UpdateSource> make_payload_source(const Payload& payload) {
    return std::make_unique<PayloadSource>(payload);
}

} // namespace parties::update::launcher
