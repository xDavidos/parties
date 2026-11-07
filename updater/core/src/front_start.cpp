#include <parties/update/front_start.h>

#include "json_util.h"

#include <algorithm>
#include <charconv>

namespace parties::update::front_start {

namespace {

constexpr std::string_view kMoveFromDiskImage = "move_from_disk_image";
constexpr std::string_view kMoveFromTranslocation = "move_from_translocation";

front_door::RecoveryOption option(std::string_view id, std::string label, std::string detail = {}) {
    return {std::string(id), std::move(label), std::move(detail)};
}

std::string explanation(std::string_view reason) {
    if (reason == "good_failed_to_start") return "Parties did not start correctly.";
    if (reason == "data_unavailable")
        return "Parties cannot open your data. Check that the data folder is reachable and not full, then try again.";
    if (reason == "data_incompatible") return "Your data was saved by a newer version of Parties than the one installed.";
    if (reason == "repairing")
        return "Some Parties files are damaged or missing. Start another version, or download Parties again.";
    if (reason == kMoveFromDiskImage || reason == kMoveFromTranslocation)
        return "Parties is running from " +
               std::string(reason == kMoveFromDiskImage ? "a disk image" : "a temporary location macOS chose for it") +
               ". It works from here, but moving it keeps it in one place.";
    return "Choose how to start Parties.";
}

} // namespace

HostArgs parse_host_args(std::span<const char* const> args, bool testing, std::string default_install_root,
                         std::string default_data_root) {
    HostArgs a{.install_root = std::move(default_install_root), .data_root = std::move(default_data_root)};
    std::optional<std::string> install_root, data_root;
    auto number = [](std::string_view text) {
        int n = 0;
        std::from_chars(text.data(), text.data() + text.size(), n);
        return n;
    };
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        auto value = [&](std::string_view name) -> std::optional<std::string> {
            if (!arg.starts_with(name)) return std::nullopt;
            return std::string(arg.substr(name.size()));
        };
        if (arg == "--") {
            a.forwarded.assign(args.begin() + i + 1, args.end());
            break;
        }
        if (arg == "--role=launcher") a.launcher = true;
        if (arg == "--recover") a.recover = true;
        if (arg == "--safe-mode") a.safe_mode = true;
        if (arg.starts_with("--ticket=")) a.ticketed = true;
        if (auto v = value("--boot-round=")) a.boot_round = number(*v);
        if (auto v = value("--after=")) a.after = number(*v);
        if (auto v = value("--build=")) a.build = *v;
        if (auto v = value("--install-seed=")) a.install_seed = *v;
        if (auto v = value("--install-root=")) install_root = *v;
        if (auto v = value("--data-root=")) data_root = *v;
        if (!testing) continue;
        for (std::string_view name : {"--install-root=", "--data-root=", "--test-namespace=", "--log-dir=", "--feed-dir="})
            if (arg.starts_with(name)) a.testing.emplace_back(arg);
        if (auto v = value("--test-namespace=")) a.test_namespace = *v;
        if (auto v = value("--self-test=")) a.self_test = *v;
        if (auto v = value("--log-dir=")) a.log_dir = *v;
    }
    if (install_root && (a.launcher || testing)) a.install_root = *install_root;
    if (data_root && (a.launcher || testing)) a.data_root = *data_root;
    return a;
}

std::vector<std::string> relaunch_arguments(const HostArgs& args, std::vector<std::string> extra, int32_t pid) {
    extra.push_back("--after=" + std::to_string(pid));
    extra.insert(extra.end(), args.testing.begin(), args.testing.end());
    extra.push_back("--");
    extra.insert(extra.end(), args.forwarded.begin(), args.forwarded.end());
    return extra;
}

std::vector<std::string> launcher_arguments(const HostArgs& args, std::string executable,
                                            const std::vector<std::string>& role_args) {
    std::vector<std::string> argv{std::move(executable), "--role=launcher", "--install-root=" + args.install_root,
                                  "--data-root=" + args.data_root};
    argv.insert(argv.end(), role_args.begin(), role_args.end());
    argv.insert(argv.end(), args.testing.begin(), args.testing.end());
    return argv;
}

std::optional<Sha256> pinned_launcher(const boot::BootContract& contract, std::string_view build) {
    if (contract.good.dir == build) return contract.good.sha256;
    if (contract.previous_good && contract.previous_good->dir == build) return contract.previous_good->sha256;
    return std::nullopt;
}

LauncherChoice::LauncherChoice(std::optional<boot::BootContract> contract, boot::BootRecord record,
                               std::optional<boot::SessionReport> session)
    : contract_(std::move(contract)), record_(std::move(record)), next_record_(record_), session_(std::move(session)) {}

std::optional<boot::RunLauncher> LauncherChoice::next() {
    if (failures_ > kMaxReChooses) return std::nullopt;
    boot::Decision d = boot::choose(contract_, record_, session_, unusable_);
    next_record_ = d.next_record;
    const auto* run = std::get_if<boot::RunLauncher>(&d.choice);
    if (!run) return std::nullopt;
    last_ = run->sha256;
    return *run;
}

void LauncherChoice::unusable() {
    if (last_) unusable_.push_back(*last_);
}

void LauncherChoice::failed() {
    record_ = next_record_ = boot::record_boot_failure(next_record_);
    session_.reset();
    ++failures_;
}

Placement classify_placement(std::string_view bundle_path, bool volume_read_only) {
    if (bundle_path.find("/AppTranslocation/") != std::string_view::npos) return Placement::Translocated;
    return volume_read_only ? Placement::ReadOnlyVolume : Placement::Installed;
}

std::optional<HostRecord> parse_host_record(std::string_view text) {
    rapidjson::Document doc;
    if (!json::parse(doc, text)) return std::nullopt;
    auto seed = json::string(doc, "seed");
    auto failures = json::u32(doc, "install_failures");
    if (!seed || !failures) return std::nullopt;
    return HostRecord{std::string(*seed), uint8_t(std::min<uint32_t>(*failures, 255))};
}

std::string write_host_record(const HostRecord& r) {
    rapidjson::StringBuffer buf;
    json::Writer w(buf);
    w.StartObject();
    json::key(w, "format"); w.Uint(1);
    json::key(w, "seed"); json::str(w, r.seed);
    json::key(w, "install_failures"); w.Uint(r.install_failures);
    w.EndObject();
    return std::string(buf.GetString(), buf.GetSize()) + "\n";
}

std::optional<std::string> parse_installed_seed(std::string_view text) {
    rapidjson::Document doc;
    if (!json::parse(doc, text)) return std::nullopt;
    auto seed = json::string(doc, "entry_sha256");
    if (!seed) return std::nullopt;
    return std::string(*seed);
}

FrontPlan plan_front(const FrontFacts& f) {
    const bool gave_up = f.record.seed == f.bundle_seed && f.record.install_failures >= kMaxSeedInstallFailures;
    const bool pending = f.bundle_seed && f.bundle_seed != f.installed_seed && !gave_up;
    if (f.launcher_reachable) return pending ? FrontPlan::ConnectSeedPending : FrontPlan::Connect;
    if (pending) return FrontPlan::InstallSeed;
    if (!f.has_contract || f.boot_round > kMaxBootRounds) return FrontPlan::SafeMode;
    return FrontPlan::ChooseLauncher;
}

HostRecord record_seed_failure(HostRecord record, std::string_view seed) {
    if (record.seed != seed) record.install_failures = 0;
    record.seed = seed;
    if (record.install_failures < 255) ++record.install_failures;
    return record;
}

Alert recovery_alert(const front_door::Recovery& r) {
    if (r.reason == kMoveFromDiskImage || r.reason == kMoveFromTranslocation)
        return {"Move Parties to your Applications folder?", explanation(r.reason), r.options};
    Alert alert{"Parties needs your help to start", explanation(r.reason), r.options};
    for (const auto& o : r.options)
        if (!o.detail.empty()) alert.message += "\n\n" + o.label + ": " + o.detail;
    alert.options.push_back(option(kOpenData, "Open the data folder"));
    alert.options.push_back(option(kQuit, "Quit"));
    return alert;
}

Alert safe_mode_alert(bool can_start_last_good) {
    Alert alert{"Parties could not start",
                can_start_last_good
                    ? "Parties could not find an installed version that starts. Your data is safe and has not been changed."
                    : "Parties could not find a version it can start. Download Parties to repair it. Your data is not "
                      "affected.",
                {}};
    if (can_start_last_good)
        alert.options.push_back(option(kStartLastGood, "Start the last working version"));
    alert.options.push_back(option(kDownload, "Download Parties"));
    alert.options.push_back(option(kOpenData, "Open the data folder"));
    alert.options.push_back(option(kQuit, "Quit"));
    return alert;
}

front_door::Recovery move_offer(Placement placement) {
    return {std::string(placement == Placement::ReadOnlyVolume ? kMoveFromDiskImage : kMoveFromTranslocation),
            {},
            {option(kMove, "Move to Applications"), option(kKeep, "Not Now")}};
}

} // namespace parties::update::front_start
