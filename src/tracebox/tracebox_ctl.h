/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_TRACEBOX_TRACEBOX_CTL_H_
#define SRC_TRACEBOX_TRACEBOX_CTL_H_

#include <string>

namespace perfetto {

// Environment variables for socket paths.
constexpr char kPerfettoProducerSockEnv[] = "PERFETTO_PRODUCER_SOCK_NAME";
constexpr char kPerfettoConsumerSockEnv[] = "PERFETTO_CONSUMER_SOCK_NAME";

// Holds the socket paths for the tracing service IPC endpoints.
struct ServiceSockets {
  std::string producer_socket;
  std::string consumer_socket;

  // Returns true if both sockets are non-empty.
  bool IsValid() const {
    return !producer_socket.empty() && !consumer_socket.empty();
  }

  std::string ToString() const {
    return "Producer Socket: " + producer_socket +
           ", Consumer Socket: " + consumer_socket;
  }
};

// Prints usage information for the `tracebox ctl` applet.
void PrintTraceboxCtlUsage();

// Main entry point for the `tracebox ctl` applet.
// Manages the lifecycle of Perfetto daemons (start/stop/status).
int TraceboxCtlMain(int argc, char** argv);

// Checks if the traced service is accessible and returns its socket paths.
// Search order: env var, Android system sockets, /run/perfetto, /tmp.
// Returns a ServiceSockets struct with empty strings if traced is not
// accessible.
ServiceSockets GetRunningSockets();

// Sets the environment variables for the tracing service socket paths.
void SetServiceSocketEnv(const ServiceSockets& sockets);

}  // namespace perfetto

#endif  // SRC_TRACEBOX_TRACEBOX_CTL_H_
