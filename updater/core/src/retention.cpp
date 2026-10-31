#include <parties/update/retention.h>

#include <algorithm>

namespace parties::update {

namespace {

bool all_digits(std::string_view s) {
    return !s.empty() && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
}

bool lower_hex(std::string_view s) {
    return std::ranges::all_of(s, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool pre_build(std::string_view stem) {
    if (!stem.starts_with("pre-")) return false;
    stem.remove_prefix(4);
    size_t g = stem.rfind("-g");
    if (g == std::string_view::npos || !all_digits(stem.substr(g + 2))) return false;
    stem = stem.substr(0, g);
    size_t dash = stem.rfind('-');
    if (dash == std::string_view::npos) return false;
    std::string_view sha = stem.substr(dash + 1);
    return sha.size() == 8 && lower_hex(sha) && Version::parse(stem.substr(0, dash)).has_value();
}

bool pre_migration(std::string_view stem) {
    constexpr std::string_view prefix = "pre-migration-v";
    if (!stem.starts_with(prefix)) return false;
    stem.remove_prefix(prefix.size());
    size_t dash = stem.find('-');
    return dash != std::string_view::npos && all_digits(stem.substr(0, dash));
}

std::vector<std::string> referenced(const SupervisorState& s) {
    std::vector<std::string> out;
    if (auto* staged = std::get_if<Staged>(&s.activation); staged && staged->restore_first)
        out.push_back(staged->restore_first->id.stem);
    if (auto* p = std::get_if<Probation>(&s.activation); p && p->pre_snapshot) out.push_back(p->pre_snapshot->id.stem);
    if (auto* r = std::get_if<Restoring>(&s.operation)) {
        out.push_back(r->from.id.stem);
        out.push_back(r->preserve_live_as.stem);
    }
    if (s.notice && s.notice->preserved) out.push_back(s.notice->preserved->stem);
    return out;
}

} // namespace

BackupKind classify_backup(std::string_view stem) {
    return pre_build(stem) || pre_migration(stem) ? BackupKind::Automatic : BackupKind::Preserved;
}

std::vector<std::string> backups_to_prune(std::span<const BackupFile> backups, const SupervisorState& state,
                                          size_t keep) {
    std::vector<const BackupFile*> automatic;
    for (const BackupFile& b : backups)
        if (classify_backup(b.stem) == BackupKind::Automatic) automatic.push_back(&b);
    std::ranges::sort(automatic, [](const BackupFile* a, const BackupFile* b) {
        return a->written != b->written ? a->written > b->written : a->stem > b->stem;
    });
    const std::vector<std::string> in_use = referenced(state);
    std::vector<std::string> out;
    for (size_t i = keep; i < automatic.size(); ++i)
        if (std::ranges::find(in_use, automatic[i]->stem) == in_use.end()) out.push_back(automatic[i]->stem);
    return out;
}

} // namespace parties::update
