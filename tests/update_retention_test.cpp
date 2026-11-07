#include <parties/update/retention.h>

#include <algorithm>
#include <iostream>
#include <string>

using namespace parties::update;
using namespace std::chrono;

namespace {

bool fail(const std::string& msg) {
    std::cerr << "update_retention_test: " << msg << "\n";
    return false;
}

const BuildId kA{*Version::parse("1.0.0.0"), *Sha256::from_hex(std::string(64, 'a'))};

std::string pre(int generation) { return "pre-" + kA.dir_name() + "-g" + std::to_string(generation); }

BackupFile file(std::string stem, int minute) { return {std::move(stem), file_clock::time_point{} + minutes(minute)}; }

SupervisorState steady() {
    BuildRecord a;
    a.id = kA;
    a.verdict = Proven{};
    return initial_state(a, Channel::Stable);
}

bool contains(const std::vector<std::string>& v, const std::string& s) { return std::ranges::find(v, s) != v.end(); }

bool test_classification() {
    bool ok = true;
    for (const std::string& stem : {pre(3), std::string("pre-migration-v1-20261031T120000Z"),
                                    std::string("pre-migration-v12-20261031T120000Z-2")})
        if (classify_backup(stem) != BackupKind::Automatic) ok &= fail(stem + " should be automatic");
    for (const std::string& stem :
         {"rolled-back-" + kA.dir_name() + "-g4", "pre-rollback-" + kA.dir_name() + "-g4",
          "pre-restore-" + kA.dir_name() + "-g4", "pre-downgrade-" + kA.dir_name() + "-g4",
          std::string("legacy-20261031T120000Z-0"), std::string("pre-uninstall-20261031T120000Z"),
          std::string("pre-migration"), std::string("pre-1.0.0.0-AAAAAAAA-g1"), std::string("pre-1.0.0-aaaaaaaa-g1"),
          std::string("pre-1.0.0.0-aaaaaaaa-g"), std::string("my-own-copy"), std::string("")})
        if (classify_backup(stem) != BackupKind::Preserved) ok &= fail("'" + stem + "' should be preserved");
    return ok;
}

bool test_keeps_newest_automatic() {
    std::vector<BackupFile> files;
    for (int i = 1; i <= 14; ++i) files.push_back(file(pre(i), i));
    files.push_back(file("pre-migration-v1-20261031T120000Z", 15));
    auto doomed = backups_to_prune(files, steady(), 10);
    std::vector<std::string> expected{pre(5), pre(4), pre(3), pre(2), pre(1)};
    if (doomed != expected) return fail("the oldest five of fifteen automatic copies should go, oldest last");
    if (!backups_to_prune(files, steady(), 15).empty()) return fail("nothing goes while under the cap");
    return true;
}

bool test_never_deletes_preserved() {
    std::vector<BackupFile> files;
    for (int i = 1; i <= 12; ++i) files.push_back(file(pre(i), 100 + i));
    for (std::string stem : {std::string("rolled-back-x"), std::string("legacy-20200101T000000Z-0"),
                             std::string("pre-uninstall-20200101T000000Z"),
                             "pre-downgrade-" + kA.dir_name() + "-g1"})
        files.push_back(file(stem, 0));
    auto doomed = backups_to_prune(files, steady(), 10);
    if (doomed != std::vector<std::string>{pre(2), pre(1)}) return fail("only the two oldest automatic copies go");
    if (backups_to_prune(files, steady(), 0).size() != 12) return fail("keep 0 deletes every automatic copy");
    for (const std::string& d : backups_to_prune(files, steady(), 0))
        if (classify_backup(d) != BackupKind::Automatic) return fail("a preserved copy was chosen: " + d);
    return true;
}

bool test_never_deletes_referenced() {
    std::vector<BackupFile> files;
    for (int i = 1; i <= 6; ++i) files.push_back(file(pre(i), i));
    const Snapshot oldest{{pre(1)}, DataInfo{1, 1}};
    const Snapshot second{{pre(2)}, DataInfo{1, 1}};
    const Snapshot third{{pre(3)}, DataInfo{1, 1}};
    bool ok = true;

    SupervisorState probation = steady();
    probation.activation = Probation{kA, kA, oldest, 0, false};
    if (contains(backups_to_prune(files, probation, 2), pre(1)))
        ok &= fail("the pre-candidate snapshot of a probation was deleted");

    SupervisorState staged = steady();
    staged.activation = Staged{kA, oldest};
    if (contains(backups_to_prune(files, staged, 2), pre(1))) ok &= fail("a staged restore_first was deleted");

    SupervisorState restoring = steady();
    restoring.operation = Restoring{oldest, second.id};
    auto doomed = backups_to_prune(files, restoring, 2);
    if (contains(doomed, pre(1)) || contains(doomed, pre(2))) ok &= fail("a restore in progress lost a copy");

    SupervisorState noticed = steady();
    noticed.notice = Notice{Notice::Kind::RolledBack, kA.version, kA.version, third.id};
    doomed = backups_to_prune(files, noticed, 2);
    if (contains(doomed, pre(3))) ok &= fail("the copy a notice names was deleted");
    if (!contains(doomed, pre(1))) ok &= fail("an unreferenced old copy was kept");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_classification();
    ok &= test_keeps_newest_automatic();
    ok &= test_never_deletes_preserved();
    ok &= test_never_deletes_referenced();
    if (ok) std::cout << "update_retention_test: all passed\n";
    return ok ? 0 : 1;
}
