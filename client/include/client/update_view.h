#pragma once

#include <client/lobby_model.h>
#include <parties/update/link.h>

#include <chrono>
#include <optional>

namespace parties::client {

void show_update_status(LobbyModel& model, const update::link::Status& status,
                        std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

std::optional<update::link::Request> update_request(const update::link::Status& status, UpdateAction action);

} // namespace parties::client
