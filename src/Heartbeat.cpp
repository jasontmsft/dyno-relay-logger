/*
 * Copyright (c) Microsoft
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 */

#include "Heartbeat.h"

#include <glog/logging.h>

namespace dynorelaylogger {

Heartbeat::Heartbeat(std::shared_ptr<GatherSystemInfo> sysinfo,
                     MessageCallback callback,
                     std::chrono::seconds interval)
    : sysinfo_(std::move(sysinfo)),
      callback_(std::move(callback)),
      interval_(interval) {}

Heartbeat::~Heartbeat() {
  stop();
}

void Heartbeat::start() {
  running_.store(true);
  thread_ = std::make_unique<std::thread>(&Heartbeat::loop, this);
  LOG(INFO) << "Heartbeat started (interval: " << interval_.count() << "s)";
}

void Heartbeat::stop() {
  if (running_.load()) {
    running_.store(false);
    cv_.notify_all();
    if (thread_ && thread_->joinable()) {
      thread_->join();
    }
    LOG(INFO) << "Heartbeat stopped";
  }
}

void Heartbeat::loop() {
  // Wait 60 seconds before first heartbeat
  {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait_for(lock, std::chrono::seconds(60), [this] { return !running_.load(); });
    if (!running_.load()) return;
  }

  // Send immediately on startup, then every interval
  while (running_.load()) {
    try {
      nlohmann::json info = sysinfo_->buildDynologSystemInfoJson(getClientIds());

      std::string msg = "::dynolog_system_info," + info.dump();
      callback_(msg);
      LOG(INFO) << "Heartbeat sent system info";

      // Send daemon stats heartbeat
      nlohmann::json hb = sysinfo_->buildDynologDaemonJson(info);

      std::string hb_msg = "::dynolog_daemon," + hb.dump();
      callback_(hb_msg);
      LOG(INFO) << "Heartbeat sent daemon stats";
    } catch (const std::exception& e) {
      LOG(WARNING) << "Heartbeat failed: " << e.what();
    }

    // Sleep for the interval, but wake up early if stopped
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait_for(lock, interval_, [this] { return !running_.load(); });

    removeOldClientIds();
  }
}

std::shared_ptr<ClientId> Heartbeat::saveClientId(std::shared_ptr<ClientId> client_id) {
  std::lock_guard<std::mutex> lock(clientIds_mutex_);
  if (clientIds_.find(client_id->exe_sha256) == clientIds_.end()) {
    clientIds_[client_id->exe_sha256] = client_id;
  } else {
    clientIds_[client_id->exe_sha256]->last_seen = client_id->last_seen;
  }

  return clientIds_[client_id->exe_sha256];
}

nlohmann::json Heartbeat::getClientIds() {
  std::lock_guard<std::mutex> lock(clientIds_mutex_);
  nlohmann::json result = nlohmann::json::array();
  for (const auto& [key, client] : clientIds_) {
    result.push_back({
        {"pid", client->pid},
        {"uid", client->uid},
        {"gid", client->gid},
        {"process_name", client->process_name},
        {"exe_sha256", client->exe_sha256},
        {"last_seen", client->last_seen},
    });
  }
  return result;
}

void Heartbeat::removeOldClientIds() {
  std::lock_guard<std::mutex> lock(clientIds_mutex_);
  std::time_t now = std::time(nullptr);
  for (auto it = clientIds_.begin(); it != clientIds_.end(); ) {
    if (now - it->second->last_seen > 3600) {
      it = clientIds_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace dynorelaylogger
