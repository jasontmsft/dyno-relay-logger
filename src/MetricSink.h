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

#include "AeHubsClient.h"
#include "MdsdClient.h"

#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace dynorelaylogger {

// Abstract interface for metric forwarding destinations.
// Implementations must be thread-safe.
class MetricSink {
 public:
  virtual ~MetricSink() = default;

  // Returns a short name for this sink (e.g. "file", "eventhubs")
  virtual std::string name() const = 0;

  // Forward a metric payload to this sink.
  // Returns true on success.
  virtual bool forward(const std::string& json_data,
                       const std::string& entity) = 0;
};

// Writes metrics to per-entity log files in a directory on disk.
class FileSink : public MetricSink {
 public:
  explicit FileSink(const std::string& output_dir = "/tmp/dyno-relay-logger");
  ~FileSink() override;

  std::string name() const override { return "file"; }

  bool forward(const std::string& json_data,
               const std::string& entity) override;

 private:
  std::string output_dir_;
  std::mutex mu_;
  std::map<std::string, std::ofstream> files_;

  std::ofstream& getFile(const std::string& entity);
};

// Forwards metrics to Azure Event Hubs via the Azure SDK AeHubsClient.
class AeHubsSink : public MetricSink {
 public:
  explicit AeHubsSink(std::shared_ptr<AeHubsClient> forwarder);

  std::string name() const override { return "aehubs"; }

  bool forward(const std::string& json_data,
               const std::string& entity) override;

 private:
  std::shared_ptr<AeHubsClient> forwarder_;
};

// Forwards metrics to mdsd via the djson UNIX-domain-socket protocol.
class MdsdSink : public MetricSink {
 public:
  explicit MdsdSink(std::shared_ptr<MdsdClient> forwarder);

  std::string name() const override { return "mdsd"; }

  bool forward(const std::string& json_data,
               const std::string& entity) override;

 private:
  std::shared_ptr<MdsdClient> forwarder_;
};

}  // namespace dynorelaylogger
