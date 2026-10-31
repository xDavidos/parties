#pragma once

// The update UI of a launcher-managed app: shows the launcher's status in the
// lobby model and turns what the user does there into link requests
// (updater contracts, "Launcher to app").

#include <client/lobby_model.h>
#include <parties/update/link.h>

#include <optional>

namespace parties::client {

void show_update_status(LobbyModel& model, const update::link::Status& status);

// nullopt when the action means nothing in this status, such as choosing the
// channel already in use or a switch the launcher does not offer.
std::optional<update::link::Request> update_request(const update::link::Status& status, UpdateAction action);

} // namespace parties::client
