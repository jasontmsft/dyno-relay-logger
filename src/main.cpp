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
#include "GatherSystemInfo.h"
#include "MetricSink.h"

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <csignal>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

DEFINE_string(logger_socket, "/var/run/dyno-relay-logger.sock",
              "Unix domain socket path for the logger listener");
DEFINE_string(mdsd_socket, "",
              "Unix domain socket path for the mdsd djson sink. If not set, defaults to discoverValidMdsdDest().");
DEFINE_bool(show_mdsd_socket_discovery_list, false,
            "Print the contents of GatherSystemInfo::kMdsdSocketCandidates "
            "and exit.");
DEFINE_int32(info_port, 1779,
             "Port for the info service (GetStats)");
DEFINE_string(forward, "",
              "Comma-separated list of forwarding sinks to enable. \n"
              "Options: file, aehubs, mdsd\n"
              "  file - forward metrics to local files in "
                "/tmp/dyno-relay-logger/ for debugging. \n"
              "  aehubs - forward metrics to Azure Event Hubs (Azure SDK). \n"
              "  mdsd - forward metrics to mdsd via the djson UNIX socket "
                "(/var/run/mdsd/dynodevlogs/default_djson.socket). \n"
              "Default: empty (drop all metrics)");
DEFINE_bool(verbose, false,
            "Echo all received metrics to stdout");

static std::unique_ptr<dynorelaylogger::DynoRelayServer> g_server;
static volatile sig_atomic_t g_shutdown_requested = 0;

void signalHandler(int signum) {
  g_shutdown_requested = 1;
  LOG(INFO) << "Received signal " << signum << ", shutting down...";
  if (g_server) {
    g_server->shutdown();
  } else {
    _exit(1);
  }
}

int main(int argc, char* argv[]) {
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  FLAGS_logtostderr = true;

  if (FLAGS_show_mdsd_socket_discovery_list) {
    for (const char* candidate :
         dynorelaylogger::GatherSystemInfo::kMdsdSocketCandidates) {
      std::cout << candidate << "\n";
    }
    return 0;
  }

  LOG(INFO) << "Starting DynoRelayLogger...";

  // Set up signal handlers for graceful shutdown
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  // Create shared stats collector (shared with logging Clients for drop tracking)
  auto stats = std::make_shared<dynorelaylogger::StatsCollector>();

  // Create shared system info (once)
  auto sysinfo = std::make_shared<dynorelaylogger::GatherSystemInfo>(stats);

  // if mdsd is enabled, verify send access before starting the server and sinks that depend on it.
  if (FLAGS_forward.find("mdsd") != std::string::npos) {
    if (FLAGS_mdsd_socket.empty()) {
      LOG(INFO) << "discovering valid Mdsd emitters...";
      sysinfo->discoverValidMdsdDest();
    } else {
      sysinfo->verifyMdsdDest(FLAGS_mdsd_socket);
    }
  }

  // if aehubs is enabled, verify send access before starting the server and sinks that depend on it.
  if (FLAGS_forward.find("aehubs") != std::string::npos) {
    sysinfo->verifyAeHubsDest();
  }

  // Parse --forward flag and construct sinks
  std::vector<std::shared_ptr<dynorelaylogger::MetricSink>> sinks;
  if (!FLAGS_forward.empty()) {
    std::istringstream ss(FLAGS_forward);
    std::string token;
    // parse --forward=file,aehubs
    while (std::getline(ss, token, ',')) {
      // Trim whitespace
      token.erase(0, token.find_first_not_of(" \t"));
      token.erase(token.find_last_not_of(" \t") + 1);

      if (token == "file") {
        LOG(INFO) << "Enabling file sink (/tmp/dyno-relay-logger/)";
        sinks.push_back(std::make_shared<dynorelaylogger::FileSink>());
      } else if (token == "aehubs") {
        if (!sysinfo->isEventHubsAvailable()) {
          LOG(WARNING) << "Skipping Azure Event Hubs sink: send access not verified";
          continue;
        }
        LOG(INFO) << "Enabling Azure Event Hubs (Azure SDK) sink...";
        auto forwarder =
            std::make_shared<dynorelaylogger::AeHubsClient>(sysinfo, stats);
        forwarder->start();
        sinks.push_back(
            std::make_shared<dynorelaylogger::AeHubsSink>(forwarder));
      } else if (token == "mdsd") {
        if (!sysinfo->isMdsdSocketAvailable()) {
          LOG(WARNING) << "Skipping mdsd sink: send access not verified";
          continue;
        }
        LOG(INFO) << "Enabling mdsd (djson) sink...";
        auto forwarder = std::make_shared<dynorelaylogger::MdsdClient>(
            stats, sysinfo->getMdsdSocketPath());
        forwarder->start();
        sinks.push_back(
            std::make_shared<dynorelaylogger::MdsdSink>(forwarder));
      } else {
        LOG(ERROR) << "Unknown sink: '" << token
                   << "' (valid: file, aehubs, mdsd)";
        return 1;
      }
    }
  }

  if (sinks.empty()) {
    LOG(INFO) << "No forwarding sinks enabled; metrics will be received and dropped";
  }

  // Start servers (blocks until shutdown)
  LOG(INFO) << "Starting logger on " << FLAGS_logger_socket;
  LOG(INFO) << "Starting info service on port " << FLAGS_info_port;
  g_server = std::make_unique<dynorelaylogger::DynoRelayServer>(
      std::move(sinks), sysinfo, FLAGS_logger_socket, FLAGS_info_port, stats);
  g_server->run();

  LOG(INFO) << "DynoRelayLogger shut down.";
  return 0;
}