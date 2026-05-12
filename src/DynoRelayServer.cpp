/*
 * Copyright (c) Microsoft
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 */

#include "DynoRelayServer.h"

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>

#include <openssl/evp.h>

#include <filesystem>

DECLARE_bool(verbose);

namespace dynorelaylogger {

using json = nlohmann::json;

// ─── InfoJsonServer (GetStats via SimpleJsonServer RPC) ─────────────────────

InfoJsonServer::InfoJsonServer(int port,
                               std::vector<std::shared_ptr<MetricSink>> sinks,
                               std::shared_ptr<StatsCollector> stats)
    : SimpleJsonServerBase(port),
      sinks_(std::move(sinks)),
      stats_(std::move(stats)) {}

std::string InfoJsonServer::processOneImpl(const std::string& request_str) {
  json request;
  try {
    request = json::parse(request_str);
  } catch (const json::parse_error&) {
    LOG(ERROR) << "Failed to parse info request: " << request_str;
    return "";
  }

  if (!request.contains("fn")) {
    LOG(ERROR) << "Info request missing 'fn' field";
    return "";
  }

  if (request["fn"] != "getStats") {
    LOG(ERROR) << "Unknown info RPC: " << request["fn"];
    return "";
  }

  auto snap = stats_->snapshot();

  auto fillWindow = [](const StatsCollector::WindowResult& w) -> json {
    return {{"lines", w.lines}, {"bytes", w.bytes}};
  };

  auto fillEntity = [&](const StatsCollector::EntityResult& e) -> json {
    return {
        {"entity", e.entity},
        {"one_minute", fillWindow(e.one_minute)},
        {"five_minutes", fillWindow(e.five_minutes)},
        {"one_hour", fillWindow(e.one_hour)},
    };
  };

  auto fillRxTx = [&](const StatsCollector::EntityRxTxResult& rxtx) -> json {
    return {
        {"entity", rxtx.entity},
        {"rx", fillEntity(rxtx.rx)},
        {"tx", fillEntity(rxtx.tx)},
        {"drops", fillEntity(rxtx.drops)},
    };
  };

  json entities = json::array();
  for (const auto& e : snap.entities) {
    entities.push_back(fillRxTx(e));
  }

  json active_sinks = json::array();
  for (const auto& sink : sinks_) {
    active_sinks.push_back(sink->name());
  }

  json response = {
      {"aggregate", fillRxTx(snap.aggregate)},
      {"entities", entities},
      {"uptime_seconds", snap.uptime_seconds},
      {"active_sinks", active_sinks},
  };

  return response.dump();
}

// ─── TCP Logger ─────────────────────────────────────────────────────────────

void DynoRelayServer::processMessage(const std::string& msg) {
  // Expect messages in the format: "::entity,json_data"
  size_t pos = msg.find(',');

  if (pos == std::string::npos) {
    LOG(WARNING) << "Invalid message format (missing comma): " << msg;
    return;
  }   

  std::string json_data = msg.substr(pos + 1);
  std::string entity = msg.substr(0, pos);
  if (entity.length() >= 2 && entity.substr(0, 2) == "::") {
    entity.erase(0, 2);
  }

  try {
    json data = json::parse(json_data);
    data["hostname"] = hostname_;
    data["location"] = location_;
    data["vmid"] = vmid_;
    data["session_uuid"] = session_uuid_;

    // Enrich GPU metrics with uuid and model from proc info
    if (entity == "dynolog_dcgm_gpu_monitor" && data.contains("device")) {
      int device = data["device"].get<int>();
      if (device >= 0 && device < static_cast<int>(GPUs_.size())) {
        data["uuid"] = GPUs_[device].uuid;
        data["model"] = GPUs_[device].model;
      }
    }

    // split out poorly formed network metrics from cpu metrics and reformat
    // into array of objects
    if (entity == "dynolog_cpu_monitor" && data.contains("cpu_util")) {
      /*
      assume data looks like this:
        {
        "cpu_guest": "0.000",
        "cpu_guest_ms": 0,
        ...
        "hostname": "aifx-clou00000F",
        ...
        "rx_bytes.eth0": 166740,
        "rx_bytes.eth1": 175450,
        ...
        "rx_drops.eth0": 0,
        "rx_drops.eth1": 0,
        ...
        "session_uuid": "fd016bd9-6ade-432e-96f1-ee9c798aca2a",
        ...
        "tx_bytes.eth0": 1255480,
        "tx_bytes.eth1": 1298734,
        ...}

        If the name prefix is "rx_" or "tx_" then transform to array.
      */
      json cpu = json::object();
      std::map<std::string, std::map<std::string, int>> netMap;

      for (auto it = data.begin(); it != data.end(); ++it) {
        const std::string& key = it.key();
        if (key.rfind("rx_", 0) == 0 || key.rfind("tx_", 0) == 0) {
          // "rx_bytes.eth0": 166740,
          size_t pos = key.find('.');

          if (pos != std::string::npos) {
            std::string first = key.substr(0, pos);
            std::string second = key.substr(pos + 1);
            netMap[second][first] = it.value();
          }
        } else {
          cpu[key] = it.value();
        }
      }

      cpu["network_interfaces"] = json::array();
      for (const auto& [outer_key, inner_map] : netMap) {
        json nic = json(inner_map);
        nic["interface"] = outer_key;
        cpu["network_interfaces"].push_back(nic);
      }

      data = cpu;
    }

    json_data = data.dump();
    int64_t bytes = static_cast<int64_t>(json_data.size());

    if (FLAGS_verbose) {
      LOG(INFO) << "[" << entity << "] " << json_data;
    }

    // Dispatch to all active sinks
    for (auto& sink : sinks_) {
      if (sink->forward(json_data, entity)) {
        stats_->recordTx(entity, bytes);
      } else {
        LOG(WARNING) << "Sink '" << sink->name() << "' failed for entity: "
                     << entity;
      }
    }
  } catch (const std::exception& e) {
    LOG(WARNING) << "Failed to process message: " << e.what();
  }
}

std::string DynoRelayServer::getExeSha256FromPid(pid_t pid) {
  std::string exe_path = (pid == 0) ? "/proc/self/exe"
                                    : "/proc/" + std::to_string(pid) + "/exe";

  FILE* file = fopen(exe_path.c_str(), "rb");
  if (!file) {
    LOG(WARNING) << "Failed to open " << exe_path << ": " << strerror(errno);
    return "";
  }

  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) {
    fclose(file);
    LOG(WARNING) << "Failed to create EVP_MD_CTX";
    return "";
  }

  if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
    EVP_MD_CTX_free(ctx);
    fclose(file);
    LOG(WARNING) << "EVP_DigestInit_ex failed";
    return "";
  }

  unsigned char buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), file)) > 0) {
    if (EVP_DigestUpdate(ctx, buf, n) != 1) {
      EVP_MD_CTX_free(ctx);
      fclose(file);
      LOG(WARNING) << "EVP_DigestUpdate failed";
      return "";
    }
  }
  fclose(file);

  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned int hash_len = 0;
  if (EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
    EVP_MD_CTX_free(ctx);
    LOG(WARNING) << "EVP_DigestFinal_ex failed";
    return "";
  }
  EVP_MD_CTX_free(ctx);

  std::ostringstream oss;
  for (unsigned int i = 0; i < hash_len; ++i) {
    oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(hash[i]);
  }
  return oss.str();
}

// Gather info about the process sending data to dyno-relay-logger including
// a SHA256 hash of the executable.
ClientId DynoRelayServer::getClientId(int client_fd) {
  ClientId result;
  struct ucred credentials;
  socklen_t ucred_length = sizeof(struct ucred);

  if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &credentials, &ucred_length) == 0) {
    result.pid = credentials.pid;
    result.uid = credentials.uid;
    result.gid = credentials.gid;
    result.last_seen = std::time(nullptr);

    // Read process name from /proc/[pid]/comm
    std::string comm_path = "/proc/" + std::to_string(credentials.pid) + "/comm";
    std::ifstream comm_file(comm_path);
    if (comm_file.is_open()) {
      std::string name;
      std::getline(comm_file, name);
      result.process_name = name;
    }
    result.exe_sha256 = getExeSha256FromPid(credentials.pid);

  } else {
    LOG(WARNING) << "getsockopt(SO_PEERCRED) failed: " << strerror(errno);
  }

  return result;
}

void DynoRelayServer::handleClient(int client_fd) {
  // Close the connection after 300 seconds of inactivity
  struct timeval tv{300, 0};
  ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  std::string buffer;
  char chunk[8192];

  std::shared_ptr<ClientId> client_id = std::make_shared<ClientId>(getClientId(client_fd));
  client_id = heartbeat_->saveClientId(client_id);

  while (running_.load()) {
    ssize_t n = ::recv(client_fd, chunk, sizeof(chunk), 0);
    if (n < 0) {
      // Timeout or error — close connection
      break;
    }
    if (n == 0) {
      // Client closed connection
      break;
    }
    buffer.append(chunk, n);

    // Extract complete messages delimited by newlines.
    size_t pos;
    while ((pos = buffer.find('\n')) != std::string::npos) {
      std::string msg = buffer.substr(0, pos);
      buffer.erase(0, pos + 1);
      if (!msg.empty()) {
        // Record rx at socket receive time, before any parsing/enrichment
        std::string entity = "unknown";
        size_t comma_pos = msg.find(',');
        if (comma_pos != std::string::npos) {
          entity = msg.substr(0, comma_pos);
          if (entity.length() >= 2 && entity.substr(0, 2) == "::") {
            entity.erase(0, 2);
          }
        }
        stats_->recordRx(entity, static_cast<int64_t>(msg.size()));

        processMessage(msg);
        client_id->last_seen = std::time(nullptr);
      }
    }
  }
  VLOG(1) << "handleClient stop";

  ::close(client_fd);
}

void DynoRelayServer::runSocketLogger() {
  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    LOG(FATAL) << "Failed to create Unix domain socket";
    return;
  }

  // Remove stale socket file if it exists
  ::unlink(socket_path_.c_str());

  // Ensure the parent directory exists
  std::filesystem::path sock_dir = std::filesystem::path(socket_path_).parent_path();
  if (!sock_dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(sock_dir, ec);
    if (ec) {
      LOG(FATAL) << "Failed to create directory " << sock_dir << ": " << ec.message();
      ::close(listen_fd_);
      return;
    }
  }

  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
  // Save old umask and set to 0 to ensure bind() uses exact bits
  mode_t old_mask = ::umask(0);

  if (::bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    LOG(FATAL) << "Failed to bind Unix domain socket at " << socket_path_;
    ::close(listen_fd_);
    return;
  }

  // Restore umask
  ::umask(old_mask);

  // Set socket file permissions: owner rw, group rw, others rw (srw-rw-rw-)
  ::chmod(socket_path_.c_str(), 0666);

  if (::listen(listen_fd_, 64) < 0) {
    LOG(FATAL) << "Failed to listen on Unix domain socket";
    ::close(listen_fd_);
    ::unlink(socket_path_.c_str());
    return;
  }

  LOG(INFO) << "Logger listening on " << socket_path_;

  while (running_.load()) {
    int client_fd = ::accept(listen_fd_, nullptr, nullptr);
    if (client_fd < 0) {
      if (running_.load()) {
        LOG(WARNING) << "accept() failed";
      }
      continue;
    }

    // Handle each client in a detached thread
    std::thread(&DynoRelayServer::handleClient, this, client_fd).detach();
  }
}

// ─── DynoRelayServer ───────────────────────────────────────────────────────

DynoRelayServer::DynoRelayServer(std::vector<std::shared_ptr<MetricSink>> sinks,
                                 std::shared_ptr<GatherSystemInfo> sysinfo,
                                 const std::string& logger_socket_path,
                                 int info_port,
                                 std::shared_ptr<StatsCollector> stats)
    : sinks_(std::move(sinks)),
      stats_(stats ? std::move(stats) : std::make_shared<StatsCollector>()),
      socket_path_(logger_socket_path),
      info_port_(info_port),
      sysinfo_(std::move(sysinfo)) {
  sysinfo_->getHostLocationId(hostname_, location_, vmid_, session_uuid_);
  GPUs_ = sysinfo_->getGpuInfo();
}

void DynoRelayServer::run() {
  running_.store(true);

  // Start JSON RPC info server in a background thread
  info_server_ = std::make_unique<InfoJsonServer>(
      info_port_, sinks_, stats_);

  if (!info_server_->initSuccessful()) {
    LOG(FATAL) << "Failed to start info server on port " << info_port_;
    return;
  }

  LOG(INFO) << "Info service listening on port " << info_port_;
  info_server_->run();

  // Start heartbeat (sends system info every 20 minutes)
  heartbeat_ = std::make_unique<Heartbeat>(
      sysinfo_,
      [this](const std::string& msg) { processMessage(msg); });
  heartbeat_->start();

  // Run Unix domain socket logger on the main thread (blocks until shutdown)
  runSocketLogger();
}

void DynoRelayServer::shutdown() {
  running_.store(false);

  // Close the logger listen socket to unblock accept()
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  ::unlink(socket_path_.c_str());

  // Stop heartbeat
  if (heartbeat_) {
    heartbeat_->stop();
  }

  // Close the info server's listen socket to unblock its accept(), then join
  if (info_server_) {
    info_server_->stop();
  }
}

}  // namespace dynorelaylogger
