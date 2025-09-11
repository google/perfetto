/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/http/http_server.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/unix_task_runner.h"
#include "perfetto/ext/base/version.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/importers/json/json_utils.h"
#include "src/trace_processor/rpc/httpd.h"
#include "src/trace_processor/rpc/rpc.h"

#include "protos/perfetto/trace_processor/trace_processor.pbzero.h"

namespace perfetto::trace_processor {
namespace {

constexpr int kBindPort = 9001;
constexpr int kMilliSecondPerMinute = 60 * 1000;
constexpr uint64_t kNanosecondPerMinute = 60 * 1000000000ULL;
constexpr uint32_t DEFAULT_TP_ID = 0;

// Sets by default the Access-Control-Allow-Origin: $origin on the following
// origins. This affects only browser clients that use CORS. Other HTTP clients
// (e.g. the python API) don't look at CORS headers.
const char* kDefaultAllowedCORSOrigins[] = {
    "https://ui.perfetto.dev",
    "http://localhost:10000",
    "http://127.0.0.1:10000",
};

void SendRpcChunk(base::HttpServerConnection* conn,
                  const void* data,
                  uint32_t len);

class Httpd : public base::HttpRequestHandler {
 public:
  explicit Httpd(std::unique_ptr<TraceProcessor>,
                 bool is_preloaded_eof,
                 size_t timeout_mins);
  ~Httpd() override;
  void Run(const std::string& listen_ip,
           int port,
           const std::vector<std::string>& additional_cors_origins);

 private:
  // HttpRequestHandler implementation.
  void OnHttpRequest(const base::HttpRequest&) override;
  void OnWebsocketMessage(const base::WebsocketMessage&) override;
  void OnHttpConnectionClosed(base::HttpServerConnection* conn) override;

  static void ServeHelpPage(const base::HttpRequest&);
  void cleanUpInactiveInstances();

  class RpcThread {
   public:
    explicit RpcThread()
        : last_accessed_ns_(
              static_cast<uint64_t>(base::GetWallTimeNs().count())) {
      rpc_thread_ = std::thread([this]() {
        // Create task runner and RPC instance in worker thread context
        base::UnixTaskRunner task_runner;
        Rpc* rpc = new Rpc();

        // Signal that initialization is complete
        {
          std::lock_guard<std::mutex> lock(init_mutex_);
          task_runner_ = &task_runner;
          rpc_ = rpc;
          initialized_ = true;
          init_cv_.notify_one();
        }

        // Run the event loop
        task_runner.Run();
      });
    }
    explicit RpcThread(std::unique_ptr<TraceProcessor> preloaded_instance,
                       bool is_preloaded_eof)
        : last_accessed_ns_(
              static_cast<uint64_t>(base::GetWallTimeNs().count())) {
      rpc_thread_ =
          std::thread([this, preloaded_instance = std::move(preloaded_instance),
                       is_preloaded_eof]() mutable {
            // Create task runner and RPC instance in worker thread context
            base::UnixTaskRunner task_runner;
            Rpc* rpc;

            // Initialize RPC with preloaded instance if provided
            if (preloaded_instance) {
              rpc = new Rpc(std::move(preloaded_instance), is_preloaded_eof);
            } else {
              rpc = new Rpc();
            }

            // Signal that initialization is complete
            {
              std::lock_guard<std::mutex> lock(init_mutex_);
              task_runner_ = &task_runner;
              rpc_ = rpc;
              initialized_ = true;
              init_cv_.notify_one();
            }

            // Run the event loop
            task_runner.Run();
          });
    }

    ~RpcThread() {
      {
        std::lock_guard<std::mutex> lock(init_mutex_);
        if (initialized_ && task_runner_) {
          task_runner_->Quit();
        }
      }
      if (rpc_thread_.joinable()) {
        rpc_thread_.join();
      }
    }

    void OnWebsocketMessage(const base::WebsocketMessage& msg) {
      // Queue the message to be processed on the worker thread
      last_accessed_ns_ = static_cast<uint64_t>(base::GetWallTimeNs().count());
      std::unique_lock<std::mutex> lock(init_mutex_);
      init_cv_.wait(lock, [this] { return initialized_; });

      if (task_runner_ && rpc_) {
        if (!rpc_->has_existing_tab) {
          rpc_->has_existing_tab = true;
        }
        task_runner_->PostTask([this, msg,
                                data = std::vector<uint8_t>(msg.data.begin(),
                                                            msg.data.end())]() {
          rpc_->SetRpcResponseFunction([msg](const void* data, uint32_t len) {
            SendRpcChunk(msg.conn, data, len);
          });
          rpc_->OnRpcRequest(data.data(), static_cast<uint32_t>(data.size()));
          rpc_->SetRpcResponseFunction(nullptr);
        });
      }
    }

    Rpc* rpc_ = nullptr;

    // Get the last accessed time in nanoseconds
    uint64_t GetLastAccessedNs() const { return last_accessed_ns_.load(); }

   private:
    std::thread rpc_thread_;  // Dedicated thread

    // These are valid only in the worker thread context
    base::UnixTaskRunner* task_runner_ = nullptr;

    // Synchronization
    std::mutex init_mutex_;
    std::condition_variable init_cv_;
    bool initialized_ = false;
    std::atomic<uint64_t> last_accessed_ns_;
  };

  uint32_t nextInstanceId = 1;

  uint32_t generateInstanceId() { return nextInstanceId++; }

  // global rpc for older uis that don't have the rpc map and for opening files
  // via trace_processor_shell
  Rpc global_trace_processor_rpc_;
  base::UnixTaskRunner task_runner_;
  base::HttpServer http_srv_;
  std::mutex websocket_rpc_mutex_;
  std::unordered_map<uint32_t, std::unique_ptr<RpcThread>> id_tp_tp_map;
  std::unordered_map<base::HttpServerConnection*, uint32_t> conn_to_id_map;
  size_t tp_timeout_mins_;
};

base::StringView Vec2Sv(const std::vector<uint8_t>& v) {
  return {reinterpret_cast<const char*>(v.data()), v.size()};
}

// Used both by websockets and /rpc chunked HTTP endpoints.
void SendRpcChunk(base::HttpServerConnection* conn,
                  const void* data,
                  uint32_t len) {
  if (data == nullptr) {
    // Unrecoverable RPC error case.
    if (!conn->is_websocket())
      conn->SendResponseBody("0\r\n\r\n", 5);
    conn->Close();
    return;
  }
  if (conn->is_websocket()) {
    conn->SendWebsocketMessage(data, len);
  } else {
    base::StackString<32> chunk_hdr("%x\r\n", len);
    conn->SendResponseBody(chunk_hdr.c_str(), chunk_hdr.len());
    conn->SendResponseBody(data, len);
    conn->SendResponseBody("\r\n", 2);
  }
}

Httpd::Httpd(std::unique_ptr<TraceProcessor> preloaded_instance,
             bool is_preloaded_eof,
             size_t timeout_mins)
    : global_trace_processor_rpc_(),  // Create empty global RPC
      http_srv_(&task_runner_, this),
      tp_timeout_mins_(timeout_mins) {
  // If we have a preloaded instance, create a ID for it and store in map
  if (!preloaded_instance->GetCurrentTraceName().empty()) {
    auto new_thread = std::make_unique<RpcThread>(std::move(preloaded_instance),
                                                  is_preloaded_eof);
    uint32_t instance_id = generateInstanceId();
    id_tp_tp_map.emplace(instance_id, std::move(new_thread));
    PERFETTO_ILOG("Preloaded trace processor assigned ID: %" PRIu32,
                  instance_id);
  }
}
Httpd::~Httpd() = default;

void Httpd::Run(const std::string& listen_ip,
                int port,
                const std::vector<std::string>& additional_cors_origins) {
  for (const auto& kDefaultAllowedCORSOrigin : kDefaultAllowedCORSOrigins) {
    http_srv_.AddAllowedOrigin(kDefaultAllowedCORSOrigin);
  }
  for (const auto& additional_cors_origin : additional_cors_origins) {
    http_srv_.AddAllowedOrigin(additional_cors_origin);
  }
  http_srv_.Start(listen_ip, port);
  PERFETTO_ILOG(
      "[HTTP] This server can be used by reloading https://ui.perfetto.dev and "
      "clicking on YES on the \"Trace Processor native acceleration\" dialog "
      "or through the Python API (see "
      "https://perfetto.dev/docs/analysis/trace-processor#python-api).");

  if (tp_timeout_mins_ > 0) {
    PERFETTO_ILOG("RPC timeout enabled: %zu minutes", tp_timeout_mins_);
  } else {
    PERFETTO_ILOG("RPC timeout disabled (timeout_mins = 0)");
  }

  // Create cleanup task using shared_ptr for proper capture
  auto cleanup_task = std::make_shared<std::function<void()>>();
  *cleanup_task = [this, cleanup_task]() {
    cleanUpInactiveInstances();
    if (tp_timeout_mins_ > 0) {
      task_runner_.PostDelayedTask(
          *cleanup_task,
          static_cast<uint32_t>(tp_timeout_mins_ * kMilliSecondPerMinute));
    }
  };

  // Initial scheduling only if timeout is enabled
  if (tp_timeout_mins_ > 0) {
    task_runner_.PostDelayedTask(
        *cleanup_task,
        static_cast<uint32_t>(tp_timeout_mins_ * kMilliSecondPerMinute));
  }

  task_runner_.Run();
}

void Httpd::OnHttpRequest(const base::HttpRequest& req) {
  base::HttpServerConnection& conn = *req.conn;
  if (req.uri == "/") {
    // If a user tries to open http://127.0.0.1:9001/ show a minimal help page.
    return ServeHelpPage(req);
  }

  static int last_req_id = 0;
  auto seq_hdr = req.GetHeader("x-seq-id").value_or(base::StringView());
  int seq_id = base::StringToInt32(seq_hdr.ToStdString()).value_or(0);

  if (seq_id) {
    if (last_req_id && seq_id != last_req_id + 1 && seq_id != 1)
      PERFETTO_ELOG("HTTP Request out of order");
    last_req_id = seq_id;
  }

  // This is the default.
  std::initializer_list<const char*> default_headers = {
      "Cache-Control: no-cache",               //
      "Content-Type: application/x-protobuf",  //
  };
  // Used by the /query and /rpc handlers for chunked replies.
  std::initializer_list<const char*> chunked_headers = {
      "Cache-Control: no-cache",               //
      "Content-Type: application/x-protobuf",  //
      "Transfer-Encoding: chunked",            //
  };

  if (req.uri == "/status") {
    protozero::HeapBuffered<protos::pbzero::RpcStatus> result;
    {
      std::lock_guard<std::mutex> lock(websocket_rpc_mutex_);
      for (const auto& entry : id_tp_tp_map) {
        uint32_t instance_id = entry.first;
        const auto& tp_rpc = entry.second;
        auto* tp_status = result->add_instances();
        tp_status->set_loaded_trace_name(tp_rpc->rpc_->GetCurrentTraceName());
        tp_status->set_human_readable_version(base::GetVersionString());
        tp_status->set_api_version(
            protos::pbzero::TRACE_PROCESSOR_CURRENT_API_VERSION);
        if (const char* version_code = base::GetVersionCode(); version_code) {
          tp_status->set_version_code(version_code);
        }
        tp_status->set_instance_id(instance_id);
        tp_status->set_last_activity(tp_rpc->GetLastAccessedNs());
        tp_status->set_has_existing_tab(tp_rpc->rpc_->has_existing_tab);
      }
    }

    // for backward compatibility, add the global instance if it has a trace
    // loaded
    if (!global_trace_processor_rpc_.GetCurrentTraceName().empty()) {
      auto* tp_status = result->add_instances();
      tp_status->set_loaded_trace_name(
          global_trace_processor_rpc_.GetCurrentTraceName());
      tp_status->set_human_readable_version(base::GetVersionString());
      if (const char* version_code = base::GetVersionCode(); version_code) {
        tp_status->set_version_code(version_code);
      }
      tp_status->set_api_version(
          protos::pbzero::TRACE_PROCESSOR_CURRENT_API_VERSION);
      tp_status->set_instance_id(DEFAULT_TP_ID);
    }
    return conn.SendResponse("200 OK", default_headers,
                             Vec2Sv(result.SerializeAsArray()));
  }

  if (base::StartsWith(req.uri.ToStdString(), "/websocket") &&
      req.is_websocket_handshake) {
    std::string path =
        req.uri.substr(strlen("/websocket"))
            .ToStdString();  // path may be "", "/", "/new", "/<id>"
    uint32_t instance_id;
    bool send_id_back = false;

    {
      std::lock_guard<std::mutex> lock(websocket_rpc_mutex_);

      if (path.empty() || path == "/") {  // Legacy /websocket endpoint
        if (id_tp_tp_map.empty()) {
          // Case 1: No instances exist, so behave like /new.
          send_id_back = true;
          uint32_t new_id = generateInstanceId();
          instance_id = new_id;
          auto new_thread = std::make_unique<RpcThread>();
          id_tp_tp_map.emplace(instance_id, std::move(new_thread));
          PERFETTO_ILOG("Legacy /websocket: creating new TP instance %" PRIu32,
                        instance_id);
        } else {
          // Case 2: Instances exist, attach to the "first" one for back-compat.
          instance_id = id_tp_tp_map.begin()->first;
          PERFETTO_ILOG(
              "Legacy /websocket: attaching to existing TP instance %" PRIu32,
              instance_id);
        }
      } else if (path == "/new") {
        // Case 3: Explicit request for a new instance.
        send_id_back = true;
        uint32_t new_id = generateInstanceId();
        instance_id = new_id;
        auto new_thread = std::make_unique<RpcThread>();
        id_tp_tp_map.emplace(instance_id, std::move(new_thread));
        PERFETTO_ILOG("New TP instance %" PRIu32 " created via /websocket/new",
                      instance_id);
      } else {
        // Case 4: Must be /websocket/<id>
        instance_id = static_cast<uint32_t>(
            std::stoul(path.substr(1)));  // Remove leading '/'
        if (id_tp_tp_map.find(instance_id) == id_tp_tp_map.end()) {
          // For the new API, if a specific instance ID is requested, it must
          // exist.
          return conn.SendResponseAndClose("404 Not Found", {});
        }
        PERFETTO_ILOG("Attaching to existing TP instance %" PRIu32,
                      instance_id);
      }

      // Associate the connection with the determined instance ID before
      // upgrading.
      conn_to_id_map.emplace(&conn, instance_id);
    }

    conn.UpgradeToWebsocket(req);

    if (send_id_back) {
      // Immediately send the new instance ID to the client upon connection.
      std::string json_response =
          "{\"id\": \"" + std::to_string(instance_id) + "\"}";
      conn.SendWebsocketMessage(json_response.c_str(), json_response.length());
    }
    return;
  }

  // --- Everything below this line is a legacy endpoint not used by the UI.
  // There are two generations of pre-websocket legacy-ness:
  // 1. The /rpc based endpoint. This is based on a chunked transfer, doing one
  //    POST request for each RPC invocation. All RPC methods are multiplexed
  //    into this one. This is still used by the python API.
  // 2. The REST API, with one endpoint per RPC method (/parse, /query, ...).
  //    This is unused and will be removed at some point.

  if (req.uri == "/rpc") {
    // Start the chunked reply.
    conn.SendResponseHeaders("200 OK", chunked_headers,
                             base::HttpServerConnection::kOmitContentLength);
    global_trace_processor_rpc_.SetRpcResponseFunction(
        [&](const void* data, uint32_t len) {
          SendRpcChunk(&conn, data, len);
        });
    // OnRpcRequest() will call SendRpcChunk() one or more times.
    global_trace_processor_rpc_.OnRpcRequest(req.body.data(), req.body.size());
    global_trace_processor_rpc_.SetRpcResponseFunction(nullptr);

    // Terminate chunked stream.
    conn.SendResponseBody("0\r\n\r\n", 5);
    return;
  }

  if (req.uri == "/parse") {
    base::Status status = global_trace_processor_rpc_.Parse(
        reinterpret_cast<const uint8_t*>(req.body.data()), req.body.size());
    protozero::HeapBuffered<protos::pbzero::AppendTraceDataResult> result;
    if (!status.ok()) {
      result->set_error(status.c_message());
    }
    return conn.SendResponse("200 OK", default_headers,
                             Vec2Sv(result.SerializeAsArray()));
  }

  if (req.uri == "/notify_eof") {
    global_trace_processor_rpc_.NotifyEndOfFile();
    return conn.SendResponse("200 OK", default_headers);
  }

  if (req.uri == "/restore_initial_tables") {
    global_trace_processor_rpc_.RestoreInitialTables();
    return conn.SendResponse("200 OK", default_headers);
  }

  // New endpoint, returns data in batches using chunked transfer encoding.
  // The batch size is determined by |cells_per_batch_|
  // |batch_split_threshold_| in query_result_serializer.h.
  // This is temporary, it will be switched to WebSockets soon.
  if (req.uri == "/query") {
    std::vector<uint8_t> response;

    // Start the chunked reply.
    conn.SendResponseHeaders("200 OK", chunked_headers,
                             base::HttpServerConnection::kOmitContentLength);

    // |on_result_chunk| will be called nested within the same callstack of the
    // rpc.Query() call. No further calls will be made once Query() returns.
    auto on_result_chunk = [&](const uint8_t* buf, size_t len, bool has_more) {
      PERFETTO_DLOG("Sending response chunk, len=%zu eof=%d", len, !has_more);
      base::StackString<32> chunk_hdr("%zx\r\n", len);
      conn.SendResponseBody(chunk_hdr.c_str(), chunk_hdr.len());
      conn.SendResponseBody(buf, len);
      conn.SendResponseBody("\r\n", 2);
      if (!has_more)
        conn.SendResponseBody("0\r\n\r\n", 5);
    };
    global_trace_processor_rpc_.Query(
        reinterpret_cast<const uint8_t*>(req.body.data()), req.body.size(),
        on_result_chunk);
    return;
  }

  if (req.uri == "/compute_metric") {
    std::vector<uint8_t> res = global_trace_processor_rpc_.ComputeMetric(
        reinterpret_cast<const uint8_t*>(req.body.data()), req.body.size());
    return conn.SendResponse("200 OK", default_headers, Vec2Sv(res));
  }

  if (req.uri == "/trace_summary") {
    std::vector<uint8_t> res = global_trace_processor_rpc_.ComputeTraceSummary(
        reinterpret_cast<const uint8_t*>(req.body.data()), req.body.size());
    return conn.SendResponse("200 OK", default_headers, Vec2Sv(res));
  }

  if (req.uri == "/enable_metatrace") {
    global_trace_processor_rpc_.EnableMetatrace(
        reinterpret_cast<const uint8_t*>(req.body.data()), req.body.size());
    return conn.SendResponse("200 OK", default_headers);
  }

  if (req.uri == "/disable_and_read_metatrace") {
    std::vector<uint8_t> res =
        global_trace_processor_rpc_.DisableAndReadMetatrace();
    return conn.SendResponse("200 OK", default_headers, Vec2Sv(res));
  }

  return conn.SendResponseAndClose("404 Not Found", default_headers);
}

void Httpd::OnWebsocketMessage(const base::WebsocketMessage& msg) {
  std::lock_guard<std::mutex> lock(websocket_rpc_mutex_);
  auto it = conn_to_id_map.find(msg.conn);
  if (it == conn_to_id_map.end()) {
    PERFETTO_ELOG("Websocket message from an un-associated connection.");
    return;
  }

  const uint32_t instance_id = it->second;
  auto id_it = id_tp_tp_map.find(instance_id);
  if (id_it == id_tp_tp_map.end()) {
    PERFETTO_ELOG(
        "Inconsistent state: conn mapped to non-existent instance id %" PRIu32,
        instance_id);
    return;
  }
  id_it->second->OnWebsocketMessage(msg);
}

void Httpd::OnHttpConnectionClosed(base::HttpServerConnection* conn) {
  std::lock_guard<std::mutex> lock(websocket_rpc_mutex_);
  auto conn_to_id_it = conn_to_id_map.find(conn);
  if (conn_to_id_it != conn_to_id_map.end()) {
    uint32_t instance_id = conn_to_id_it->second;
    auto id_to_tp_it = id_tp_tp_map.find(instance_id);
    if (id_to_tp_it != id_tp_tp_map.end()) {
      if (id_to_tp_it->second->rpc_->GetCurrentTraceName().empty()) {
        id_tp_tp_map.erase(id_to_tp_it);
      } else {
        id_to_tp_it->second->rpc_->has_existing_tab = false;
      }
    }
    conn_to_id_map.erase(conn_to_id_it);
  }
}

void Httpd::cleanUpInactiveInstances() {
  std::lock_guard<std::mutex> lock(websocket_rpc_mutex_);

  if (tp_timeout_mins_ == 0) {
    // Timeout disabled
    return;
  }

  uint64_t kInactivityNs =
      static_cast<uint64_t>(tp_timeout_mins_) * kNanosecondPerMinute;
  uint64_t now = static_cast<uint64_t>(base::GetWallTimeNs().count());

  for (auto it = id_tp_tp_map.begin(); it != id_tp_tp_map.end();) {
    const uint32_t instance_id = it->first;
    uint64_t last_accessed = it->second->GetLastAccessedNs();

    if (now - last_accessed > kInactivityNs) {
      PERFETTO_ILOG(
          "Cleaning up inactive RPC instance: %" PRIu32
          " (inactive for %.1f minutes)",
          instance_id,
          static_cast<double>(now - last_accessed) / (60.0 * 1000000000.0));
      // Remove from conn_to_id_map as well
      for (auto conn_it = conn_to_id_map.begin();
           conn_it != conn_to_id_map.end();) {
        if (conn_it->second == instance_id) {
          conn_it = conn_to_id_map.erase(conn_it);
        } else {
          ++conn_it;
        }
      }

      // Remove the RpcThread
      it = id_tp_tp_map.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace

void RunHttpRPCServer(std::unique_ptr<TraceProcessor> preloaded_instance,
                      bool is_preloaded_eof,
                      const std::string& listen_ip,
                      const std::string& port_number,
                      const std::vector<std::string>& additional_cors_origins,
                      size_t timeout_mins) {
  Httpd srv(std::move(preloaded_instance), is_preloaded_eof, timeout_mins);
  std::optional<int> port_opt = base::StringToInt32(port_number);
  std::string ip = listen_ip.empty() ? "localhost" : listen_ip;
  int port = port_opt.has_value() ? *port_opt : kBindPort;
  srv.Run(ip, port, additional_cors_origins);
}

void Httpd::ServeHelpPage(const base::HttpRequest& req) {
  static const char kPage[] = R"(
Perfetto Trace Processor RPC Server


This service can be used in two ways:

1. Open or reload https://ui.perfetto.dev/

It will automatically try to connect and use the server on localhost:9001 when
available. Click YES when prompted to use Trace Processor Native Acceleration
in the UI dialog.
See https://perfetto.dev/docs/visualization/large-traces for more.


2. Python API.

Example: perfetto.TraceProcessor(addr='localhost:9001')
See https://perfetto.dev/docs/analysis/trace-processor#python-api for more.


For questions:
https://perfetto.dev/docs/contributing/getting-started#community
)";

  std::initializer_list<const char*> headers{"Content-Type: text/plain"};
  req.conn->SendResponse("200 OK", headers, kPage);
}

}  // namespace perfetto::trace_processor
