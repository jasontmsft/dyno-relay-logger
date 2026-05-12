/*
 * Copyright (c) Microsoft
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 */

#pragma once

#include "GatherSystemInfo.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace dynorelaylogger {

struct ClientId {
  pid_t pid = -1;
  uid_t uid = 0;
  gid_t gid = 0;
  std::string process_name;
  std::string exe_sha256;
  std::time_t last_seen = 0;
};

// Periodically sends system info as an internal message via a callback.
class Heartbeat {
 public:
  using MessageCallback = std::function<void(const std::string&)>;

  Heartbeat(std::shared_ptr<GatherSystemInfo> sysinfo,
            MessageCallback callback,
            std::chrono::seconds interval = std::chrono::seconds(1200));

  ~Heartbeat();

  void start();
  void stop();

  // Save a client ID to the map if its exe_sha256 is not already present
  std::shared_ptr<ClientId> saveClientId(std::shared_ptr<ClientId> client_id);

  // Get all client IDs as a JSON object
  nlohmann::json getClientIds();

  // Remove client IDs not seen in over 3600 seconds
  void removeOldClientIds();

 private:
  void loop();

  std::shared_ptr<GatherSystemInfo> sysinfo_;
  MessageCallback callback_;
  std::chrono::seconds interval_;
  std::atomic<bool> running_{false};
  std::unique_ptr<std::thread> thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::map<std::string, std::shared_ptr<ClientId>> clientIds_;
  std::mutex clientIds_mutex_;
};

}  // namespace dynorelaylogger
