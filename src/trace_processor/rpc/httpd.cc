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
#include "perfetto/ext/base/lock_free_task_runner.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/unix_task_runner.h"
#include "perfetto/ext/base/uuid.h"
#include "perfetto/ext/base/version.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/rpc/httpd.h"
#include "src/trace_processor/rpc/rpc.h"

#include "protos/perfetto/trace_processor/trace_processor.pbzero.h"

namespace perfetto::trace_processor {
namespace {

constexpr int kBindPort = 9001;
constexpr int kMilliSecondPerMinute = 60 * 1000;
constexpr uint64_t kNanosecondPerMinute = 60 * 1000000000ULL;
constexpr const char kDefaultTpUuid[] = "";

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
  void CleanUpInactiveInstances();

  class RpcThread {
   public:
    explicit RpcThread(Httpd* httpd)
        : httpd_(httpd),
          last_accessed_ns_(
              static_cast<uint64_t>(base::GetWallTimeNs().count())) {
      rpc_thread_ = std::thread([this]() {
        // Create task runner and RPC instance in worker thread context
        base::UnixTaskRunner task_runner;
        std::unique_ptr<Rpc> rpc = std::make_unique<Rpc>();

        // Signal that initialization is complete
        {
          std::lock_guard<std::mutex> lock(init_mutex_);
          task_runner_ = &task_runner;
          rpc_ = std::move(rpc);
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
        if (!rpc_->IsAttached()) {
          rpc_->SetAttachedState(true);
        }
        task_runner_->PostTask([this, msg,
                                data = std::vector<uint8_t>(msg.data.begin(),
                                                            msg.data.end())]() {
          rpc_->SetRpcResponseFunction(
              [this, conn = msg.conn](const void* data, uint32_t len) {
                std::lock_guard<std::mutex> lock(httpd_->websocket_rpc_mutex_);
                if (httpd_->conn_to_id_map.count(conn)) {
                  // only send the chunk if the connection is still registered
                  // by the httpd.
                  SendRpcChunk(conn, data, len);
                }
              });
          rpc_->OnRpcRequest(data.data(), static_cast<uint32_t>(data.size()));
          rpc_->SetRpcResponseFunction(nullptr);
        });
      }
    }

    std::unique_ptr<Rpc> rpc_;

    // Get the last accessed time in nanoseconds
    uint64_t GetLastAccessedNs() const { return last_accessed_ns_.load(); }

   private:
    Httpd* const httpd_;
    std::thread rpc_thread_;  // Dedicated thread

    // These are valid only in the worker thread context
    base::UnixTaskRunner* task_runner_ = nullptr;

    // Synchronization
    std::mutex init_mutex_;
    std::condition_variable init_cv_;
    bool initialized_ = false;
    std::atomic<uint64_t> last_accessed_ns_;
  };

  // global rpc for older uis that don't have the rpc map and for opening files
  // via trace_processor_shell
  Rpc global_trace_processor_rpc_;
  base::UnixTaskRunner task_runner_;
  base::HttpServer http_srv_;
  std::mutex websocket_rpc_mutex_;
  std::unordered_map<std::string, std::unique_ptr<RpcThread>> id_to_tp_map;
  std::unordered_map<base::HttpServerConnection*, std::string> conn_to_id_map;
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
    : global_trace_processor_rpc_(Rpc(std::move(preloaded_instance), is_preloaded_eof)),  // Create empty global RPC
      http_srv_(&task_runner_, this),
      tp_timeout_mins_(timeout_mins) {}
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

  // Create a self-repeating cleanup task every tp_timeout_mins to clean up
  // inactive instances. repeatedly check for any instances that have been
  // inactive for more than tp_timeout_mins_ and clean them up. If
  // tp_timeout_mins_ is 0, auto cleanup is disabled.
  auto cleanup_task = std::make_shared<std::function<void()>>();
  *cleanup_task = [this, cleanup_task]() {
    CleanUpInactiveInstances();
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
      for (const auto& entry : id_to_tp_map) {
        std::string tp_uuid = entry.first;
        const auto& tp_rpc = entry.second;
        auto* tp_status = result->add_instances();
        tp_status->set_loaded_trace_name(tp_rpc->rpc_->GetCurrentTraceName());
        tp_status->set_human_readable_version(base::GetVersionString());
        tp_status->set_api_version(
            protos::pbzero::TRACE_PROCESSOR_CURRENT_API_VERSION);
        if (const char* version_code = base::GetVersionCode(); version_code) {
          tp_status->set_version_code(version_code);
        }
        tp_status->set_instance_uuid(tp_uuid);
        tp_status->set_inactivity_ns(
            static_cast<uint64_t>(base::GetWallTimeNs().count()) -
            tp_rpc->GetLastAccessedNs());
        tp_status->set_is_attached(tp_rpc->rpc_->IsAttached());
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
      tp_status->set_instance_uuid(kDefaultTpUuid);
    }

    // adding legacy support for older uis
    auto id_to_tp_it = id_to_tp_map.begin();
    if (id_to_tp_it != id_to_tp_map.end()) {
      result->set_loaded_trace_name(
          id_to_tp_it->second->rpc_->GetCurrentTraceName());
      result->set_human_readable_version(base::GetVersionString());
      if (const char* version_code = base::GetVersionCode(); version_code) {
        result->set_version_code(version_code);
      }
      result->set_api_version(
          protos::pbzero::TRACE_PROCESSOR_CURRENT_API_VERSION);
    } else {
      result->set_loaded_trace_name(
          global_trace_processor_rpc_.GetCurrentTraceName());
      result->set_human_readable_version(base::GetVersionString());
      if (const char* version_code = base::GetVersionCode(); version_code) {
        result->set_version_code(version_code);
      }
      result->set_api_version(
          protos::pbzero::TRACE_PROCESSOR_CURRENT_API_VERSION);
    }

    return conn.SendResponse("200 OK", default_headers,
                             Vec2Sv(result.SerializeAsArray()));
  }

  if (base::StartsWith(req.uri.ToStdString(), "/websocket") &&
      req.is_websocket_handshake) {
    std::string path = req.uri.substr(strlen("/websocket"))
                           .ToStdString();  // path may be "", "/", "/<id>"
    std::string instance_uuid;
    // bool send_id_back = false;

    {
      std::lock_guard<std::mutex> lock(websocket_rpc_mutex_);

      if (path.empty() ||
          path == "/") {  // If the WS handshake does not come with an UUID,
                          // register it with the global tp with the Legacy
                          // /websocket endpoint
        PERFETTO_ILOG(
            "Legacy /websocket endpoint: connecting to global trace "
            "processor instance");
        instance_uuid = kDefaultTpUuid;
        // } else if (path == "/new") {
        //   // Case 3: Explicit request for a new instance.
        //   send_id_back = true;
        //   base::Uuid new_uuid = base::Uuidv4();
        //   instance_uuid = new_uuid.ToPrettyString();
        //   auto new_thread = std::make_unique<RpcThread>(this);
        //   id_to_tp_map.emplace(instance_uuid, std::move(new_thread));
        //   PERFETTO_ILOG("New TP instance %s created via /websocket/new",
        //                 instance_uuid.c_str());
      } else {
        // if an ID's provided, create a new TP in a separate thread and
        // register it in the map.
        std::string parsed_uuid = path.substr(1);
        if (parsed_uuid.empty()) {
          return conn.SendResponseAndClose("404 Not Found", {});
        }
        instance_uuid = parsed_uuid;
        if (id_to_tp_map.find(instance_uuid) == id_to_tp_map.end()) {
          //   if no such thread exists, create one.
          instance_uuid = path;
          auto new_thread = std::make_unique<RpcThread>(this);
          id_to_tp_map.emplace(instance_uuid, std::move(new_thread));
          PERFETTO_ILOG("New TP instance %s created via /websocket/<tp_uuid>",
                        instance_uuid.c_str());
        } else {
          PERFETTO_ILOG("Attaching to existing TP instance %s",
                        instance_uuid.c_str());
        }
      }

      // Associate the connection with the determined instance ID before
      // upgrading.
      conn_to_id_map.emplace(&conn, instance_uuid);
    }

    conn.UpgradeToWebsocket(req);

    // if (send_id_back) {
    //   // Immediately send a status message to the client upon connection.
    //   // This follows the pattern of TPM_GET_STATUS responses.
    //   // We need to wrap the TraceProcessorRpc message into a
    //   // TraceProcessorRpcStream to be compliant with the RPC protocol on the
    //   // wire.
    //   protozero::HeapBuffered<protos::pbzero::TraceProcessorRpcStream>
    //   stream; auto* rpc = stream->add_msg(); rpc->set_seq(0);  // This is the
    //   first message, seq can be 0.
    //   rpc->set_response(protos::pbzero::TraceProcessorRpc::TPM_GET_STATUS);

    //   auto* status = rpc->set_status();
    //   // For a new instance, the trace name is empty.
    //   status->set_loaded_trace_name("");
    //   status->set_human_readable_version(base::GetVersionString());
    //   if (const char* version_code = base::GetVersionCode(); version_code) {
    //     status->set_version_code(version_code);
    //   }
    //   status->set_api_version(
    //       protos::pbzero::TRACE_PROCESSOR_CURRENT_API_VERSION);
    //   status->set_instance_uuid(instance_uuid);
    //   req.conn->SendWebsocketMessage(
    //       Vec2Sv(stream.SerializeAsArray()).data(),
    //       static_cast<uint32_t>(stream.SerializeAsArray().size()));
    // }
    return;
  }

  if (req.uri == "/close") {
    // TODO: fix the UI /close request to send UUID instead of instance ID
    const std::string instance_uuid = req.body.ToStdString();
    if (instance_uuid.empty()) {
      return conn.SendResponseAndClose("400 Bad Request", default_headers);
    }

    {
      std::lock_guard<std::mutex> lock(websocket_rpc_mutex_);
      auto id_to_tp_it = id_to_tp_map.find(instance_uuid);
      if (id_to_tp_it == id_to_tp_map.end()) {
        return conn.SendResponseAndClose("404 Not Found", default_headers);
      }
      id_to_tp_map.erase(id_to_tp_it);
      for (auto conn_to_id_it = conn_to_id_map.begin();
           conn_to_id_it != conn_to_id_map.end();) {
        if (conn_to_id_it->second == instance_uuid) {
          auto conn_to_close = conn_to_id_it->first;
          conn_to_id_it = conn_to_id_map.erase(conn_to_id_it);
          conn_to_close->Close();
        } else {
          ++conn_to_id_it;
        }
      }
    }
    PERFETTO_ILOG("Closed and removed TP instance %s", instance_uuid.c_str());
    return conn.SendResponseAndClose("200 OK", default_headers);
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
  auto conn_to_id_it = conn_to_id_map.find(msg.conn);
  if (conn_to_id_it == conn_to_id_map.end() || conn_to_id_it->second ==
                                                  kDefaultTpUuid) {
    // Connection not registered, which can happen if we are using an older
    // version of the UI. In this case we use the global RPC because there is no
    // UUID associated with it.
    global_trace_processor_rpc_.SetRpcResponseFunction(
        [&](const void* data, uint32_t len) {
          SendRpcChunk(msg.conn, data, len);
        });
    // OnRpcRequest() will call SendRpcChunk() one or more times.
    global_trace_processor_rpc_.OnRpcRequest(msg.data.data(), msg.data.size());
    global_trace_processor_rpc_.SetRpcResponseFunction(nullptr);
    return;
  }

  std::string instance_uuid = conn_to_id_it->second;
  auto id_to_tp_it = id_to_tp_map.find(instance_uuid);
  if (id_to_tp_it == id_to_tp_map.end()) {
    PERFETTO_ELOG(
        "Inconsistent state: conn mapped to non-existent instance id %s",
        instance_uuid.c_str());
    return;
  }
  id_to_tp_it->second->OnWebsocketMessage(msg);
}

void Httpd::OnHttpConnectionClosed(base::HttpServerConnection* conn) {
  std::lock_guard<std::mutex> lock(websocket_rpc_mutex_);
  auto conn_to_id_it = conn_to_id_map.find(conn);
  if (conn_to_id_it != conn_to_id_map.end()) {
    std::string instance_uuid = conn_to_id_it->second;
    auto id_to_tp_it = id_to_tp_map.find(instance_uuid);
    if (id_to_tp_it != id_to_tp_map.end()) {
      if (id_to_tp_it->second->rpc_->GetCurrentTraceName().empty()) {
        id_to_tp_map.erase(id_to_tp_it);
      } else {
        id_to_tp_it->second->rpc_->SetAttachedState(false);
      }
    }
    conn_to_id_map.erase(conn_to_id_it);
  }
}

void Httpd::CleanUpInactiveInstances() {
  std::lock_guard<std::mutex> lock(websocket_rpc_mutex_);

  if (tp_timeout_mins_ == 0) {
    // Timeout disabled
    return;
  }

  uint64_t kInactivityNs =
      static_cast<uint64_t>(tp_timeout_mins_) * kNanosecondPerMinute;
  uint64_t now = static_cast<uint64_t>(base::GetWallTimeNs().count());

  for (auto it = id_to_tp_map.begin(); it != id_to_tp_map.end();) {
    const std::string instance_uuid = it->first;
    uint64_t last_accessed = it->second->GetLastAccessedNs();

    if (now - last_accessed > kInactivityNs) {
      PERFETTO_ILOG(
          "Cleaning up inactive RPC instance: %s"
          " (inactive for %.1f minutes)",
          instance_uuid.c_str(),
          static_cast<double>(now - last_accessed) / (kNanosecondPerMinute));
      // Remove from conn_to_id_map as well
      for (auto conn_to_id_it = conn_to_id_map.begin();
           conn_to_id_it != conn_to_id_map.end();) {
        if (conn_to_id_it->second == instance_uuid) {
          auto conn_to_close = conn_to_id_it->first;
          conn_to_id_it = conn_to_id_map.erase(conn_to_id_it);
          conn_to_close->Close();
        } else {
          ++conn_to_id_it;
        }
      }

      // Remove the RpcThread
      it = id_to_tp_map.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace

void RunHttpRPCServer(Rpc& rpc,
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
