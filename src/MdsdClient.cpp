/*
 * Copyright (c) Microsoft
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 */

#include "MdsdClient.h"

#include <glog/logging.h>
#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <random>
#include <utility>

namespace dynorelaylogger {

namespace {

using json = nlohmann::json;

// djson ack codes
constexpr unsigned long kAckSuccess           = 0;
constexpr unsigned long kAckUnknownSchemaId   = 2;
constexpr unsigned long kAckDecodeError       = 3;
constexpr unsigned long kAckInvalidSource     = 4;
constexpr unsigned long kAckDuplicateSchemaId = 5;

const char* AckName(unsigned long code) {
  switch (code) {
    case kAckSuccess:           return "ACK_SUCCESS";
    case kAckUnknownSchemaId:   return "ACK_UNKNOWN_SCHEMA_ID";
    case kAckDecodeError:       return "ACK_DECODE_ERROR";
    case kAckInvalidSource:     return "ACK_INVALID_SOURCE";
    case kAckDuplicateSchemaId: return "ACK_DUPLICATE_SCHEMA_ID";
    default:                    return "UNKNOWN";
  }
}

// Single-field payload schema, shared across all entities.
json PayloadSchema() {
  return json::array({json::array({"payload", "FT_STRING"})});
}

}  // namespace

const std::map<std::string, MdsdClient::Route>& MdsdClient::entityRoutes() {
  // Stable per-entity (source, schema_id) routes. mdsd disallows underscores
  // in source names, so the dynolog entity names are mapped to camelCase.
  // mdsd keys schema registration by (source, schema_id); changing either
  // value forces re-registration.
  static const std::map<std::string, Route> kTable = {
      {"dynolog_daemon",           {"dynoDaemon",          0xDA010001ULL}},
      {"dynolog_cpu_monitor",      {"dynoCpuMonitor",      0xDA010002ULL}},
      {"dynolog_system_info",      {"dynoSystemInfo",      0xDA010003ULL}},
      {"dynolog_dcgm_gpu_monitor", {"dynoDcgmGpuMonitor",  0xDA010004ULL}},
  };
  return kTable;
}

MdsdClient::MdsdClient(std::shared_ptr<StatsCollector> stats,
                       std::string socket_path)
    : socket_path_(std::move(socket_path)),
      stats_(std::move(stats)) {}

MdsdClient::~MdsdClient() {
  stop();
}

void MdsdClient::start() {
  LOG(INFO) << "mdsd: socket " << socket_path_;
  running_.store(true);
  sender_thread_ = std::make_unique<std::thread>(&MdsdClient::senderLoop, this);
  LOG(INFO) << "mdsd client started (batch flush every "
            << kFlushIntervalSeconds << "s, max queue depth "
            << kMaxQueueDepth << ")";
}

void MdsdClient::stop() {
  if (!running_.exchange(false)) return;
  cv_.notify_all();
  if (sender_thread_ && sender_thread_->joinable()) {
    sender_thread_->join();
  }
  disconnect_();
  LOG(INFO) << "mdsd client stopped";
}

bool MdsdClient::forwardMetrics(const std::string& json_data,
                                const std::string& entity) {
  if (entityRoutes().find(entity) == entityRoutes().end()) {
    // mdsd intentionally does not route this entity; report success so the
    // dispatcher doesn't log a sink failure.
    return true;
  }

  std::lock_guard<std::mutex> lock(queue_mu_);
  auto& queue = queues_[entity];
  if (queue.size() >= kMaxQueueDepth) {
    if (stats_) {
      stats_->recordDrop(entity, json_data.size());
    }
    VLOG(1) << "mdsd: queue full for " << entity << ", dropping message";
    return false;
  }
  queue.push_back(json_data);
  return true;
}

bool MdsdClient::connect_() {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    LOG(ERROR) << "mdsd: socket() failed: " << std::strerror(errno);
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (socket_path_.size() >= sizeof(addr.sun_path)) {
    LOG(ERROR) << "mdsd: socket path too long: " << socket_path_;
    ::close(fd);
    return false;
  }
  std::memcpy(addr.sun_path, socket_path_.data(), socket_path_.size());

  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    LOG(WARNING) << "mdsd: connect(" << socket_path_
                 << ") failed: " << std::strerror(errno);
    ::close(fd);
    return false;
  }

  fd_ = fd;
  registered_schemas_.clear();
  LOG(INFO) << "mdsd: connected to " << socket_path_;
  return true;
}

void MdsdClient::disconnect_() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  registered_schemas_.clear();
}

bool MdsdClient::writeAll(const char* data, size_t len) {
  while (len > 0) {
    ssize_t n = ::write(fd_, data, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      LOG(WARNING) << "mdsd: write() failed: " << std::strerror(errno);
      return false;
    }
    data += n;
    len  -= static_cast<size_t>(n);
  }
  return true;
}

bool MdsdClient::readAck(std::uint64_t& out_msg_id, unsigned long& out_code) {
  std::string line;
  line.reserve(32);
  while (true) {
    char c;
    ssize_t n = ::read(fd_, &c, 1);
    if (n == 0) {
      LOG(WARNING) << "mdsd: EOF before ack (partial: '" << line << "')";
      return false;
    }
    if (n < 0) {
      if (errno == EINTR) continue;
      LOG(WARNING) << "mdsd: read() failed: " << std::strerror(errno);
      return false;
    }
    if (c == '\n') break;
    line.push_back(c);
    if (line.size() > 128) {
      LOG(WARNING) << "mdsd: ack line too long: " << line;
      return false;
    }
  }
  auto colon = line.find(':');
  if (colon == std::string::npos) {
    LOG(WARNING) << "mdsd: malformed ack: " << line;
    return false;
  }
  try {
    out_msg_id = std::stoull(line.substr(0, colon));
    out_code   = std::stoul(line.substr(colon + 1));
  } catch (const std::exception& e) {
    LOG(WARNING) << "mdsd: failed to parse ack '" << line << "': " << e.what();
    return false;
  }
  return true;
}

bool MdsdClient::sendOne(const std::string& entity,
                         const std::string& json_data,
                         unsigned long* out_ack_code) {
  auto it = entityRoutes().find(entity);
  if (it == entityRoutes().end()) return true;  // shouldn't reach here
  const std::string& source = it->second.source;
  const std::uint64_t schema_id = it->second.schema_id;
  const std::uint64_t msg_id = next_msg_id_.fetch_add(1);

  json frame = json::array();
  frame.push_back(source);
  frame.push_back(msg_id);
  frame.push_back(schema_id);
  if (registered_schemas_.count(schema_id)) {
    frame.push_back(nullptr);
  } else {
    frame.push_back(PayloadSchema());
  }
  frame.push_back(json::array({json_data}));

  std::string body = frame.dump();
  if (body.empty() || body.size() > kMaxMsgDataSize) {
    LOG(WARNING) << "mdsd: frame for " << entity << " out of size range ("
                 << body.size() << " bytes), dropping";
    if (stats_) stats_->recordDrop(entity, json_data.size());
    return true;  // not a socket error
  }

  std::string header = std::to_string(body.size()) + "\n";
  if (!writeAll(header.data(), header.size()) ||
      !writeAll(body.data(), body.size())) {
    return false;
  }

  std::uint64_t ack_id;
  unsigned long ack_code;
  if (!readAck(ack_id, ack_code)) {
    return false;
  }
  if (out_ack_code) *out_ack_code = ack_code;

  switch (ack_code) {
    case kAckSuccess:
    case kAckDuplicateSchemaId:
      registered_schemas_.insert(schema_id);
      bytes_sent_.fetch_add(body.size(), std::memory_order_relaxed);
      break;
    case kAckUnknownSchemaId:
      // Server doesn't know this schema_id — we'll resend the schema next time.
      registered_schemas_.erase(schema_id);
      LOG(WARNING) << "mdsd: " << entity << "(" << schema_id << ") msg " << ack_id << ": "
                   << AckName(ack_code) << " (will re-register schema)";
      break;
    default:
      LOG(WARNING) << "mdsd: " << entity << "(" << schema_id << ") msg " << ack_id << ": "
                   << AckName(ack_code) << " (code=" << ack_code << ")";
      break;
  }
  return true;
}

void MdsdClient::flushEntity(const std::string& entity,
                             std::deque<std::string>& queue) {
  while (!queue.empty()) {
    const auto& msg = queue.front();
    if (!sendOne(entity, msg)) {
      // Socket error — the fd will be closed below. Drop the rest of this
      // batch; next flush cycle will reconnect.
      LOG(WARNING) << "mdsd: send failed for " << entity << ", dropping "
                   << queue.size() << " queued messages";
      if (stats_) {
        for (const auto& m : queue) stats_->recordDrop(entity, m.size());
      }
      disconnect_();
      queue.clear();
      return;
    }
    queue.pop_front();
  }
}

bool MdsdClient::sendSync(const std::string& json_data,
                          const std::string& entity) {
  LOG(INFO) << "mdsd sendSync: probing send access via " << socket_path_
            << " for entity '" << entity << "'";

  if (entityRoutes().find(entity) == entityRoutes().end()) {
    LOG(WARNING) << "mdsd sendSync: entity '" << entity
                 << "' is not in the routing table";
    return false;
  }

  if (fd_ < 0) {
    if (!connect_()) {
      return false;
    }
  }

  unsigned long ack_code = kAckSuccess;
  bool socket_ok = sendOne(entity, json_data, &ack_code);

  if (!socket_ok) {
    LOG(WARNING) << "mdsd sendSync: socket-level send/ack failure";
    return false;
  }
  if (ack_code != kAckSuccess) {
    LOG(WARNING) << "mdsd sendSync: ack code " << ack_code << " ("
                 << AckName(ack_code) << "), not ACK_SUCCESS";
    return false;
  }

  LOG(INFO) << "mdsd sendSync: ACK_SUCCESS — send access verified";
  return true;
}

void MdsdClient::senderLoop() {
  // Random initial delay to avoid thundering herd across fleet restarts.
  {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, kFlushIntervalSeconds - 1);
    int jitter = dist(gen);
    LOG(INFO) << "mdsd: initial jitter delay " << jitter << "s";
    std::unique_lock<std::mutex> lock(cv_mu_);
    cv_.wait_for(lock, std::chrono::seconds(jitter),
                 [this] { return !running_.load(); });
  }

  while (running_.load()) {
    int wait_s = kFlushIntervalSeconds;
    if (fd_ < 0 && connect_backoff_s_ > 0) {
      wait_s = std::max(wait_s, connect_backoff_s_);
    }
    {
      std::unique_lock<std::mutex> lock(cv_mu_);
      cv_.wait_for(lock, std::chrono::seconds(wait_s),
                   [this] { return !running_.load(); });
    }
    if (!running_.load()) break;

    if (fd_ < 0) {
      if (!connect_()) {
        connect_backoff_s_ = std::min(
            connect_backoff_s_ == 0 ? kInitialBackoffSeconds
                                    : connect_backoff_s_ * 2,
            kMaxBackoffSeconds);
        LOG(WARNING) << "mdsd: connect failed; retrying in "
                     << connect_backoff_s_ << "s";
        // Drop anything queued — we'd just lose it next cycle anyway, and
        // letting queues grow under a dead connection masks the outage.
        std::lock_guard<std::mutex> lock(queue_mu_);
        for (auto& [entity, q] : queues_) {
          if (stats_) {
            for (const auto& m : q) stats_->recordDrop(entity, m.size());
          }
          q.clear();
        }
        continue;
      }
      connect_backoff_s_ = 0;
    }

    std::map<std::string, std::deque<std::string>> snapshot;
    {
      std::lock_guard<std::mutex> lock(queue_mu_);
      snapshot.swap(queues_);
    }
    for (auto& [entity, queue] : snapshot) {
      if (queue.empty()) continue;
      flushEntity(entity, queue);
      if (fd_ < 0) break;  // disconnected mid-flush; rest will be dropped
    }
  }

  // Final flush on shutdown (best-effort; only if still connected).
  if (fd_ >= 0) {
    std::map<std::string, std::deque<std::string>> remaining;
    {
      std::lock_guard<std::mutex> lock(queue_mu_);
      remaining.swap(queues_);
    }
    for (auto& [entity, queue] : remaining) {
      if (queue.empty()) continue;
      flushEntity(entity, queue);
      if (fd_ < 0) break;
    }
  }
}

}  // namespace dynorelaylogger
