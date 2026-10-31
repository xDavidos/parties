#include "file_io.h"
#include "update_source.h"

#include <windows.h>

namespace parties::update::launcher {

namespace {

class LocalDirSource final : public UpdateSource {
public:
    explicit LocalDirSource(fs::path dir) : dir_(std::move(dir)) {}

    std::expected<std::vector<uint8_t>, SourceError> fetch_manifest(Channel channel,
                                                                   const std::atomic<bool>&) override {
        fs::path file = dir_ / (std::string(channel_name(channel)) + ".manifest.json");
        auto bytes = read_file(file);
        if (!bytes) return std::unexpected(SourceError{"cannot read " + utf8(file)});
        if (bytes->size() > kMaxEnvelopeBytes) return std::unexpected(SourceError{utf8(file) + " is too large"});
        return std::vector<uint8_t>(bytes->begin(), bytes->end());
    }

    std::expected<void, SourceError> fetch_file(const FileEntry& file, const fs::path& dest,
                                                const Progress& progress, const std::atomic<bool>& cancel) override {
        if (cancel.load()) return std::unexpected(SourceError{"cancelled"});
        fs::path from = dir_ / "files" / file.sha256.to_hex();
        if (!CopyFileW(from.c_str(), dest.c_str(), TRUE))
            return std::unexpected(SourceError{"cannot copy " + utf8(from) + ": " + last_error_text()});
        if (progress) progress(file.size);
        return {};
    }

private:
    fs::path dir_;
};

} // namespace

std::unique_ptr<UpdateSource> make_local_dir_source(std::filesystem::path dir) {
    return std::make_unique<LocalDirSource>(std::move(dir));
}

} // namespace parties::update::launcher
