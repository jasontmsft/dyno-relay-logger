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

#include <nlohmann/json.hpp>

#include <list>
#include <memory>
#include <string>
#include <vector>

namespace dynorelaylogger {

// GPU information structure
struct GpuProcInfo {
  std::string model;
  int irq = 0;
  std::string uuid;
  std::string bios;
  std::string bus_type;
  std::string bus_location;
  int device_minor = 0;
  std::string firmware;
};

// Network interface information structure
struct NicInfo {
  std::string interface;
  std::string device_id;
  std::string firmware;
  std::string numa_node;
  std::string speed;
  std::string state;
  std::string sys_image_guid;
};

// Gathers Azure system info, GPU info, and Event Hub SAS keys
class GatherSystemInfo {
 public:
  static constexpr const char* kMdsdSocketCandidates[] = {
      "/var/run/mdsd/dynolog/default_djson.socket",
      "/var/run/mdsd/aisc/default_djson.socket",
      "/var/run/mdsd/inframdsdaccount/default_djson.socket",
      "/var/run/mdsd/default_djson.socket",
  };

  GatherSystemInfo(std::shared_ptr<StatsCollector> stats);

  std::vector<GpuProcInfo> getGpuInfo() const;
  std::vector<NicInfo> getNicInfo() const;
  nlohmann::json getAzureSystemInfo() const;
  void getHostLocationId(std::string& hostname, std::string& location,
                         std::string& vmid, std::string& session_uuid) const;
  bool isDcgmAvailable() const;
  bool isEventHubsAvailable() const;
  bool isMdsdSocketAvailable() const;
  std::string getMdsdSocketPath() const;
  std::string getClientId() const;
  std::string getEhNamespace() const;
  void verifyMdsdDest(const std::string& mdsd_socket_path);
  void verifyAeHubsDest();
  // Probes each path in kMdsdSocketCandidates in order; returns the first
  // socket that accepts a send-access probe, or "" if none responded.
  void discoverValidMdsdDest();
  nlohmann::json buildDynologSystemInfoJson(const nlohmann::json& client_info);
  nlohmann::json buildDynologDaemonJson(const nlohmann::json& info);

 private:
  std::shared_ptr<StatsCollector> stats_;
  std::string eh_namespace_;
  std::string hostname_;
  std::string location_;
  std::string vmid_;
  std::vector<GpuProcInfo> gpus_;
  std::vector<NicInfo> nics_;
  bool dcgm_available_ = false;
  bool eh_send_available_ = false;
  bool mdsd_send_available_ = false;
  std::string mdsd_socket_path_;
  nlohmann::json azure_system_info_;
  std::string session_uuid_;
  std::string self_exe_sha256_;
  std::string client_id_;

  void gatherNvidiaGpuProcInfo();
  void gatherNicInfo();
  void readLocalMachineIds();
  void fetchImdsMetadata(const std::list<std::string>& computeKeys);
  void gatherAzureMetadata();
  bool verifyAzureEventHubsSendAccess();
  bool verifyMdsdSendAccess(const std::string& mdsd_socket_path);
  std::string parseNvidiaInfoValue(const std::string& s);
  static size_t curlWriteFunction(void* contents, size_t size, size_t nmemb,
                              std::string* data);
};

}  // namespace dynorelaylogger
