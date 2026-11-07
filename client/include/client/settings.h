#pragma once

#include <parties/types.h>

#include <client/settings_schema.h>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct sqlite3;

namespace parties::client {

struct SavedServer {
    int         id = 0;
    std::string name;
    std::string host;
    int         port = 7800;
    std::string fingerprint;   // TOFU cert fingerprint
    std::string last_username;
    std::string password;
};

struct Identity {
    std::string seed_phrase;
    SecretKey   secret_key{};
    PublicKey   public_key{};
};

struct ArchivedIdentity {
    Identity    identity;
    std::string archived_at;
    std::string reason;
};

struct SettingsPaths {
    std::filesystem::path db;
    std::filesystem::path backups_dir;
};

class Settings {
public:
    Settings();
    ~Settings();

    enum class OpenStatus { Ready, TooNew, Unavailable };
    struct OpenResult {
        OpenStatus  status = OpenStatus::Unavailable;
        std::string reason;
    };

    OpenResult open(const SettingsPaths& paths);
    bool is_open() const { return db_ != nullptr; }
    void close();

    // --- Identity ---
    // Append-only: replacing or deleting an identity moves it to the archive.
    bool has_identity();
    bool save_identity(const std::string& seed_phrase,
                       const SecretKey& sk, const PublicKey& pk);
    bool delete_identity();
    std::optional<Identity> load_identity();
    std::vector<ArchivedIdentity> archived_identities();
    std::string get_fingerprint();

    // --- TOFU certificate store ---
    enum class TofuResult { Trusted, Unknown, Mismatch };
    TofuResult check_fingerprint(const std::string& host, int port,
                                  const std::string& fingerprint);
    bool trust_fingerprint(const std::string& host, int port,
                           const std::string& fingerprint);

    // --- Saved servers ---
    bool save_server(const std::string& name, const std::string& host, int port,
                     const std::string& fingerprint, const std::string& last_username,
                     const std::string& password = "");
    std::vector<SavedServer> get_saved_servers();
    bool update_server_username(int id, const std::string& username);
    bool delete_server(int id);

    // --- Resumption tickets (0-RTT) ---
    bool save_resumption_ticket(const std::string& host, int port,
                                const uint8_t* data, size_t len);
    std::vector<uint8_t> load_resumption_ticket(const std::string& host, int port);
    bool delete_resumption_ticket(const std::string& host, int port);

    // --- Preferences ---
    bool set_pref(const std::string& key, const std::string& value);
    std::optional<std::string> get_pref(const std::string& key);

private:
    friend struct SettingsTestSeam;

    OpenResult open_with(const SettingsPaths& paths, std::span<const schema::Migration> migrations);
    bool exec(const char* sql, std::string& error);
    bool apply_migration(const schema::Migration& migration, std::string& error);

    sqlite3* db_ = nullptr;
};

} // namespace parties::client
