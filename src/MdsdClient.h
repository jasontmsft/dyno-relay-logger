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

#include "StatsCollector.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace dynorelaylogger {

// Client for mdsd's dynamic-JSON (djson) UNIX-domain-socket protocol.
// Forwards each enqueued payload as a one-field FT_STRING "payload" djson
// message. Per-entity queueing, periodic flush, and reconnect-with-backoff
// mirror the AeHubsClient pattern.
//
// Wire format per message:
//   <size>\n<json>
// where <json> is the array [source, msg_id, schema_id, schema_or_null, [payload_string]].
// The server replies with "<msg_id>:<code>\n" after each frame.
class MdsdClient {
 public:
  static constexpr size_t kMaxQueueDepth = 100;
  static constexpr int kFlushIntervalSeconds = 30;
  static constexpr size_t kMaxMsgDataSize = 128 * 1024 - 1;
  static constexpr const char* kDefaultSocketPath =
      "/var/run/mdsd/dynodevlogs/default_djson.socket";

  explicit MdsdClient(std::shared_ptr<StatsCollector> stats,
                      std::string socket_path = kDefaultSocketPath);
  ~MdsdClient();

  void start();
  void stop();

  // Enqueue a JSON payload for the named entity. Returns false if the queue
  // is full (caller's recordDrop happens automatically). Entities not in the
  // mdsd routing table are silently accepted (return true) to avoid noisy
  // dispatcher warnings for sinks that intentionally don't route them.
  bool forwardMetrics(const std::string& json_data,
                      const std::string& entity);

  // One-shot synchronous send: opens a fresh connection, sends a single
  // djson frame for entity, blocks on the ack, closes. Returns true only on
  // ACK_SUCCESS. Intended for send-access probes before start() — NOT safe
  // to call concurrently with the batched sender thread.
  bool sendSync(const std::string& json_data, const std::string& entity);

 private:
  void senderLoop();
  void flushEntity(const std::string& entity,
                   std::deque<std::string>& queue);

  bool connect_();
  void disconnect_();

  // Build the djson frame, write it, and read the ack.
  // Returns false on a socket-level error (and closes the fd in that case).
  // Application-level ack codes (decode error, invalid source, etc.) are
  // logged but treated as successful sends so we continue to next message.
  // If out_ack_code is non-null, the parsed ack code is written there.
  bool sendOne(const std::string& entity, const std::string& json_data,
               unsigned long* out_ack_code = nullptr);

  bool writeAll(const char* data, size_t len);
  bool readAck(std::uint64_t& out_msg_id, unsigned long& out_code);

  // Routing table: entity -> (mdsd source name, schema_id). mdsd source names
  // must not contain underscores, so the dynolog-style entity names are mapped
  // to camelCase. Schema is the same single-field FT_STRING "payload" schema
  // for all entities; only the id differs so mdsd tracks them as separate sources.
  struct Route {
    std::string source;
    std::uint64_t schema_id;
  };
  static const std::map<std::string, Route>& entityRoutes();

  std::string socket_path_;
  std::shared_ptr<StatsCollector> stats_;

  // Sender-thread-only state (no locking needed).
  int fd_{-1};
  std::set<std::uint64_t> registered_schemas_;
  int connect_backoff_s_{0};
  static constexpr int kInitialBackoffSeconds = 5;
  static constexpr int kMaxBackoffSeconds = 300;

  // Producer-side state.
  std::mutex queue_mu_;
  std::map<std::string, std::deque<std::string>> queues_;

  std::unique_ptr<std::thread> sender_thread_;
  std::mutex cv_mu_;
  std::condition_variable cv_;
  std::atomic<bool> running_{false};

  std::atomic<std::uint64_t> next_msg_id_{1};
  std::atomic<std::uint64_t> bytes_sent_{0};
};

}  // namespace dynorelaylogger
