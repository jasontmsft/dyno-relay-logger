/*
 * Copyright (c) Microsoft
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 */

#include "GatherSystemInfo.h"
#include "DynoRelayServer.h"
#include "MdsdClient.h"

#include <azure/identity.hpp>
#include <azure/messaging/eventhubs/producer_client.hpp>
#include <curl/curl.h>
#include <glog/logging.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <list>
#include <map>
#include <sstream>
#include <unistd.h>

#include <openssl/evp.h>

#include <uuid/uuid.h>

namespace dynorelaylogger {

GatherSystemInfo::GatherSystemInfo(std::shared_ptr<StatsCollector> stats)
    : stats_(std::move(stats)) {
  uuid_t uuid;
  char uuid_str[37];
  uuid_generate(uuid);
  uuid_unparse_lower(uuid, uuid_str);
  session_uuid_ = uuid_str;
  self_exe_sha256_ = DynoRelayServer::getExeSha256FromPid(0);

  gatherNvidiaGpuProcInfo();
  gatherNicInfo();
  gatherAzureMetadata();
  while (vmid_.empty() || location_.empty()) {
    LOG(INFO) << "Waiting for Azure metadata to be available...";
    gatherAzureMetadata();
    for (int i = 0; i < 100 && (vmid_.empty() || location_.empty()); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  } 
  eh_send_available_ = false;
}

void GatherSystemInfo::verifyLoggingAvailable() {
  eh_send_available_ = verifyAzureEventHubsSendAccess();
  mdsd_send_available_ = verifyMdsdSendAccess();
}

std::vector<GpuProcInfo> GatherSystemInfo::getGpuInfo() const {
  return gpus_;
}

std::vector<NicInfo> GatherSystemInfo::getNicInfo() const {
  return nics_;
}

bool GatherSystemInfo::isDcgmAvailable() const {
  return dcgm_available_;
}

bool GatherSystemInfo::isEventHubsAvailable() const {
  return eh_send_available_;
}

bool GatherSystemInfo::isMdsdAvailable() const {
  return mdsd_send_available_;
}

std::string GatherSystemInfo::getClientId() const {
  return client_id_;
}

std::string GatherSystemInfo::getEhNamespace() const {
  return eh_namespace_;
}

bool GatherSystemInfo::verifyAzureEventHubsSendAccess() {
  std::string fqns = "" + eh_namespace_ + ".servicebus.windows.net";

  LOG(INFO) << "Verifying managed identity client_id_=" << client_id_
             << " by sending test event to " << fqns;  

  try {
    auto credential =
      std::make_shared<Azure::Identity::ManagedIdentityCredential>(client_id_);

    Azure::Messaging::EventHubs::ProducerClient producer(
        fqns, "dynolog_daemon", credential);

    Azure::Messaging::EventHubs::EventDataBatchOptions batchOptions;
    auto batch = producer.CreateBatch(batchOptions);

    nlohmann::json testPayload = buildDynologDaemonJson(azure_system_info_);
    Azure::Messaging::EventHubs::Models::EventData event(testPayload.dump());
    batch.TryAdd(event);

    producer.Send(batch);

    LOG(INFO) << "Managed identity verified: sent test event to dynolog_daemon";
    return true;
  } catch (const Azure::Core::Credentials::AuthenticationException& e) {
    LOG(WARNING) << "Managed identity auth failed (client_id_=" << client_id_
                 << "): " << e.what();
  } catch (const Azure::Core::RequestFailedException& e) {
    LOG(WARNING) << "Managed identity lacks RBAC permissions (client_id_="
                 << client_id_ << "): " << e.what();
  } catch (const std::exception& e) {
    LOG(WARNING) << "Failed to verify managed identity credential (client_id_="
                 << client_id_ << "): " << e.what();
  }
  return false;
}

bool GatherSystemInfo::verifyMdsdSendAccess() {
  LOG(INFO) << "Verifying mdsd djson send access";
  MdsdClient client(stats_);
  std::string payload = buildDynologDaemonJson(azure_system_info_).dump();
  return client.sendSync(payload, "dynolog_daemon");
}

nlohmann::json GatherSystemInfo::getAzureSystemInfo() const {
  return azure_system_info_;
}

nlohmann::json GatherSystemInfo::buildDynologSystemInfoJson(
    const nlohmann::json& client_info) {
  nlohmann::json info = getAzureSystemInfo();
  info["t"] = std::time(nullptr);

  info["nvidia_gpu_count"] = static_cast<int>(gpus_.size());
  nlohmann::json gpu_array = nlohmann::json::array();
  for (int i = 0; i < static_cast<int>(gpus_.size()); ++i) {
    const auto& gpu = gpus_[i];
    gpu_array.push_back({
        {"number", i},
        {"model", gpu.model},
        {"uuid", gpu.uuid},
        {"irq", gpu.irq},
        {"bios", gpu.bios},
        {"bus_type", gpu.bus_type},
        {"bus_location", gpu.bus_location},
        {"device_minor", gpu.device_minor},
        {"firmware", gpu.firmware}
    });
  }
  info["nvidia_gpu_info"] = gpu_array;

  nlohmann::json nic_array = nlohmann::json::array();
  for (int i = 0; i < static_cast<int>(nics_.size()); ++i) {
    const auto& nic = nics_[i];
    nic_array.push_back({
        {"number", i},
        {"interface", nic.interface},
        {"device_id", nic.device_id},
        {"firmware", nic.firmware},
        {"numa_node", nic.numa_node},
        {"speed", nic.speed},
        {"state", nic.state},
        {"sys_image_guid", nic.sys_image_guid}
    });
  }
  info["network_interfaces"] = nic_array;
  info["client_info"] = client_info;

  return info;
}

nlohmann::json GatherSystemInfo::buildDynologDaemonJson(
    const nlohmann::json& info) {
  auto snap = stats_->snapshot();
  nlohmann::json hb;
  hb["machine_id"] = info.value("machine_id", "");
  hb["hostname"] = hostname_;
  hb["location"] = location_;
  hb["vmid"] = vmid_;
  hb["session_uuid"] = session_uuid_;
  hb["t"] = std::time(nullptr);
  hb["log_data_rx_bytes_1m"] = snap.aggregate.rx.one_minute.bytes;
  hb["log_data_rx_bytes_5m"] = snap.aggregate.rx.five_minutes.bytes;
  hb["log_data_rx_bytes_1h"] = snap.aggregate.rx.one_hour.bytes;
  hb["log_data_tx_bytes_1m"] = snap.aggregate.tx.one_minute.bytes;
  hb["log_data_tx_bytes_5m"] = snap.aggregate.tx.five_minutes.bytes;
  hb["log_data_tx_bytes_1h"] = snap.aggregate.tx.one_hour.bytes;

  return hb;
}

std::string GatherSystemInfo::parseNvidiaInfoValue(const std::string& line) {
  std::string ret;
  size_t colonPos = line.find(':');
  if (colonPos != std::string::npos) {
    ret = line.substr(colonPos + 1);
    ret.erase(0, ret.find_first_not_of(" \t"));
    ret.erase(ret.find_last_not_of(" \t") + 1);
  }
  return ret;
}

void GatherSystemInfo::getHostLocationId(std::string& hostname,
                                         std::string& location,
                                         std::string& vmid,
                                         std::string& session_uuid) const {
  hostname = hostname_;
  location = location_;
  vmid = vmid_;
  session_uuid = session_uuid_;
}

void GatherSystemInfo::readLocalMachineIds() {
  // Read machine-id
  try {
    std::ifstream machine_id_file("/etc/machine-id");
    std::string machineId;
    std::getline(machine_id_file, machineId);
    azure_system_info_["machine_id"] = machineId;
  } catch (std::exception& e) {
    LOG(WARNING) << "error reading /etc/machine-id: " << e.what();
  }

  // Read product UUID
  try {
    std::ifstream uuid_file("/sys/class/dmi/id/product_uuid");
    std::string productUuid;
    std::getline(uuid_file, productUuid);
    azure_system_info_["product_uuid"] = productUuid;
  } catch (std::exception& e) {
    LOG(WARNING) << "error reading /sys/class/dmi/id/product_uuid: " << e.what();
  }
}

// Helper for curl: write callback to accumulate response data
size_t GatherSystemInfo::curlWriteFunction(void* contents, size_t size,
                                       size_t nmemb, std::string* data) {
  data->append((char*)contents, size * nmemb);
  return size * nmemb;
}

void GatherSystemInfo::fetchImdsMetadata(
    const std::list<std::string>& computeKeys) {
  try {
    std::string responseData;
    CURL* curl = curl_easy_init();
    if (curl) {
      struct curl_slist* headers = nullptr;
      headers = curl_slist_append(headers, "Metadata: true");
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteFunction);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseData);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
      curl_easy_setopt(curl, CURLOPT_URL,
          "http://169.254.169.254/metadata/instance?api-version=2025-04-07");

      CURLcode res = curl_easy_perform(curl);

      if (res == CURLE_OK) {
        nlohmann::json jsonData = nlohmann::json::parse(responseData);

        nlohmann::json compute =
            jsonData.value("compute", nlohmann::json::object());
        if (compute.size()) {
          location_ = compute["location"];
          hostname_ = compute["name"];
          vmid_ = compute["vmId"];

          for (const auto& k : computeKeys) {
            azure_system_info_[k] = compute[k];
          }

          nlohmann::json osProfile =
              compute.value("osProfile", nlohmann::json::object());
          if (osProfile.size()) {
            hostname_ = osProfile.value("computerName", hostname_);
          }
          azure_system_info_["hostname"] = hostname_;
        }

        nlohmann::json network =
            jsonData.value("network", nlohmann::json::object());
        if (network.size()) {
          nlohmann::json interface =
              network.value("interface", nlohmann::json::object());
          if (interface.size() && interface.is_array()) {
            azure_system_info_["mac_address"] =
                interface[0].value("macAddress", "");
            nlohmann::json ipv4 =
                interface[0].value("ipv4", nlohmann::json::object());
            if (ipv4.size()) {
              nlohmann::json ipAddress =
                  ipv4.value("ipAddress", nlohmann::json::object());
              if (ipAddress.size() && ipAddress.is_array()) {
                azure_system_info_["public_ipv4_ip"] =
                    ipAddress[0].value("publicIpAddress", "");
                azure_system_info_["private_ipv4_ip"] =
                    ipAddress[0].value("privateIpAddress", "");
              }
            }
          }
        }
      }

      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
    }
  } catch (std::exception& e) {
    LOG(WARNING) << "Azure metadata fetch failed: " << e.what();
  }
}

void GatherSystemInfo::gatherAzureMetadata() {
  std::list<std::string> computeKeys = {
      "azEnvironment",    "location",          "name",
      "placementGroupId", "provider",          "resourceGroupName",
      "resourceId",       "subscriptionId",    "tags",
      "tagsList",         "version",           "vmId",
      "vmScaleSetName",   "vmSize"};

  for (const auto& k : computeKeys) {
    azure_system_info_[k] = "";
  }
  azure_system_info_["t"] = std::time(nullptr);

  readLocalMachineIds();
  fetchImdsMetadata(computeKeys);

  // Parse dyno-client-id from Azure tags (semicolon-separated "key:value" pairs)
  if (azure_system_info_.contains("tagsList") &&
      azure_system_info_["tagsList"].is_array()) {
    for (const auto& tag : azure_system_info_["tagsList"]) {
      if (tag.contains("name") && tag.contains("value")) {
        std::string name = tag["name"].get<std::string>();
        if (name == "dyno-client-id") {
          client_id_ = tag["value"].get<std::string>();
          LOG(INFO) << "dyno-client-id = " << client_id_;
        } else if (name == "dyno-event-hubs-namespace") {
          eh_namespace_ = tag["value"].get<std::string>();
          LOG(INFO) << "dyno-event-hubs-namespace = " << eh_namespace_;
        }
      }
    }
  }

  // Fallback: use gethostname() if IMDS didn't provide a hostname
  if (hostname_.empty()) {
    char buf[256];
    if (::gethostname(buf, sizeof(buf)) == 0) {
      hostname_ = buf;
    }
    LOG(INFO) << "hostname_(gethostname) = " << hostname_;
  }
}

void GatherSystemInfo::gatherNvidiaGpuProcInfo() {
  std::vector<GpuProcInfo> readGpus;
  const std::string nvidiaDcgmServiceFile =
      "/usr/lib/systemd/system/nvidia-dcgm.service";
  const std::string procPath = "/proc/driver/nvidia/gpus";

  if (std::filesystem::exists(nvidiaDcgmServiceFile)) {
    dcgm_available_ = true;
  }

  try {
    if (!std::filesystem::exists(procPath)) {
      LOG(INFO) << "NVIDIA proc filesystem not found at " << procPath;
      return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(procPath)) {
      if (entry.is_directory()) {
        std::string infoPath = entry.path().string() + "/information";
        std::ifstream infoFile(infoPath);

        if (infoFile.is_open()) {
          GpuProcInfo gpui;
          std::string line;
          while (std::getline(infoFile, line)) {
            if (line.find("GPU UUID:") != std::string::npos)
              gpui.uuid = parseNvidiaInfoValue(line);
            if (line.find("Video BIOS:") != std::string::npos)
              gpui.bios = parseNvidiaInfoValue(line);
            if (line.find("Bus Type:") != std::string::npos)
              gpui.bus_type = parseNvidiaInfoValue(line);
            if (line.find("Bus Location:") != std::string::npos)
              gpui.bus_location = parseNvidiaInfoValue(line);
            if (line.find("Device Minor:") != std::string::npos)
              gpui.device_minor = std::stoi(parseNvidiaInfoValue(line));
            if (line.find("GPU Firmware:") != std::string::npos)
              gpui.firmware = parseNvidiaInfoValue(line);
            if (line.find("IRQ:") != std::string::npos)
              gpui.irq = std::stoi(parseNvidiaInfoValue(line));
            if (line.find("Model:") != std::string::npos)
              gpui.model = parseNvidiaInfoValue(line);
          }
          if (!gpui.uuid.empty()) {
            readGpus.push_back(gpui);
          }
        }
      }
    }
  } catch (const std::exception& e) {
    LOG(WARNING) << "Error reading NVIDIA proc filesystem: " << e.what();
  }

  if (!readGpus.empty()) {
    int num = readGpus.size();
    gpus_.resize(num);
    for (auto& g : readGpus) {
      if (g.device_minor < num) {
        gpus_[g.device_minor] = g;
      }
    }
  } else {
    dcgm_available_ = false;
  }

  for (int i = 0; i < static_cast<int>(gpus_.size()); ++i) {
    LOG(INFO) << "GPU[" << i << "]: uuid=" << gpus_[i].uuid
              << " model=" << gpus_[i].model;
  }
}

void GatherSystemInfo::gatherNicInfo() {
  const std::filesystem::path base_path = "/sys/class/net";
  const std::string nvidia_vendor_id = "0x15b3";

  auto read_sysfs = [](const std::filesystem::path& path) -> std::string {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string val;
    std::getline(f, val);
    val.erase(val.find_last_not_of(" \t\n\r") + 1);
    val.erase(0, val.find_first_not_of(" \t\n\r"));
    return val;
  };

  auto first_entry = [](const std::filesystem::path& dir) -> std::string {
    try {
      for (const auto& entry : std::filesystem::directory_iterator(dir))
        return entry.path().filename().string();
    } catch (...) {}
    return "";
  };

  try {
    if (!std::filesystem::exists(base_path)) {
      LOG(INFO) << "Network sysfs not found at " << base_path;
      return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(base_path)) {
      std::string interface = entry.path().filename().string();
      std::filesystem::path dev_path = base_path / interface / "device";

      std::string vendor = read_sysfs(dev_path / "vendor");
      if (vendor != nvidia_vendor_id)
        continue;

      std::string dev_id = read_sysfs(dev_path / "device");
      if (dev_id.empty())
        continue;

      NicInfo nic;
      nic.interface = interface;
      nic.device_id = dev_id;

      // Firmware version and sys_image_guid from infiniband sysfs
      std::filesystem::path ib_path = dev_path / "infiniband";
      std::string ib_dev = first_entry(ib_path);
      if (!ib_dev.empty()) {
        std::string val = read_sysfs(ib_path / ib_dev / "fw_ver");
        if (!val.empty()) nic.firmware = val;
        val = read_sysfs(ib_path / ib_dev / "sys_image_guid");
        if (!val.empty() && val != "0000:0000:0000:0000") nic.sys_image_guid = val;
      }

      // NUMA node
      nic.numa_node = read_sysfs(dev_path / "numa_node");

      // Link speed: prefer IB rate for IB interfaces, fall back to ethtool speed
      if (interface.rfind("ib", 0) == 0 && !ib_dev.empty()) {
        std::string val = read_sysfs(ib_path / ib_dev / "ports" / "1" / "rate");
        if (!val.empty()) nic.speed = val;
      }
      if (nic.speed.empty()) {
        std::string val = read_sysfs(base_path / interface / "speed");
        if (!val.empty()) nic.speed = val + " Mbps";
      }

      // Operational state
      nic.state = read_sysfs(base_path / interface / "operstate");
      if (nic.state.empty()) nic.state = "unknown";

      nics_.push_back(nic);
      LOG(INFO) << "NIC: " << nic.interface
                << " dev=" << nic.device_id
                << " fw=" << nic.firmware << " " << nic.speed
                << " " << nic.state;
    }
  } catch (const std::exception& e) {
    LOG(WARNING) << "Error reading NIC sysfs info: " << e.what();
  }
}

}  // namespace dynorelaylogger
