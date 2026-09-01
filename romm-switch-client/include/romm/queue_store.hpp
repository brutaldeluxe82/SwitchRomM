#pragma once

#include "romm/config.hpp"
#include "romm/status.hpp"
#include <string>

namespace romm {

// Persisted queue snapshot path (on SD).
inline constexpr const char* kQueueStatePath = "sdmc:/switch/TicromM/queue_state.json";

// Save active queue entries (pending/running) to disk.
bool saveQueueState(const Status& status, std::string& outError, const std::string& path = kQueueStatePath);

// Load queue entries from disk and append non-duplicate, non-completed items.
bool loadQueueState(Status& status, const Config& cfg, std::string& outError, const std::string& path = kQueueStatePath);

} // namespace romm
