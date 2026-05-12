/*
 * Copyright (c) Microsoft
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 */

#include "MetricSink.h"
#include <glog/logging.h>
#include <filesystem>

namespace dynorelaylogger {

// ─── FileSink ───────────────────────────────────────────────────────────────

FileSink::FileSink(const std::string& output_dir) : output_dir_(output_dir) {
  std::filesystem::create_directories(output_dir_);
  LOG(INFO) << "FileSink writing to " << output_dir_;
}

FileSink::~FileSink() {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto& [entity, file] : files_) {
    if (file.is_open()) file.close();
  }
}

std::ofstream& FileSink::getFile(const std::string& entity) {
  auto it = files_.find(entity);
  if (it != files_.end() && it->second.is_open()) {
    return it->second;
  }
  // Sanitize entity to prevent path traversal
  std::string safe_entity;
  for (char c : entity) {
    if (std::isalnum(c) || c == '_' || c == '-') {
      safe_entity += c;
    }
  }
  if (safe_entity.empty()) {
    safe_entity = "unknown";
  }
  std::string path = output_dir_ + "/" + safe_entity + ".log";
  auto& f = files_[entity];
  f.open(path, std::ios::app);
  if (!f.is_open()) {
    LOG(ERROR) << "FileSink: failed to open " << path;
  }
  return f;
}

bool FileSink::forward(const std::string& json_data,
                       const std::string& entity) {
  std::lock_guard<std::mutex> lock(mu_);
  auto& f = getFile(entity);
  if (!f.is_open()) return false;
  f << json_data << "\n";
  f.flush();
  return true;
}

// ─── AeHubsSink ─────────────────────────────────────────────────────────────

AeHubsSink::AeHubsSink(std::shared_ptr<AeHubsClient> forwarder)
    : forwarder_(std::move(forwarder)) {}

bool AeHubsSink::forward(const std::string& json_data,
                          const std::string& entity) {
  return forwarder_->forwardMetrics(json_data, entity);
}

// ─── MdsdSink ───────────────────────────────────────────────────────────────

MdsdSink::MdsdSink(std::shared_ptr<MdsdClient> forwarder)
    : forwarder_(std::move(forwarder)) {}

bool MdsdSink::forward(const std::string& json_data,
                       const std::string& entity) {
  return forwarder_->forwardMetrics(json_data, entity);
}

}  // namespace dynorelaylogger
