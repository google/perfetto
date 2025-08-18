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

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <filesystem>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/http/http_server.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/unix_task_runner.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/rpc/httpd.h"
#include "src/trace_processor/rpc/rpc.h"

#include "protos/perfetto/trace_processor/trace_processor.pbzero.h"

namespace perfetto::trace_processor {
namespace {

constexpr int kBindPort = 9001;

constexpr char kStorageDir[] = "./storage";

// Sets the Access-Control-Allow-Origin: $origin on the following origins.
// This affects only browser clients that use CORS. Other HTTP clients (e.g. the
// python API) don't look at CORS headers.
const char* kAllowedCORSOrigins[] = {
    "https://ui.perfetto.dev",
    "http://localhost:10000",
    "http://127.0.0.1:10000",
    "http://0.0.0.0:10000",
};

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

class WebSocketRpcThread {
 public:
  explicit WebSocketRpcThread(base::HttpServerConnection* conn) : conn_(conn) {
    rpc_thread_ = std::thread([this]() {
      // Create task runner and RPC instance in worker thread context
      base::UnixTaskRunner task_runner;
      Rpc rpc;
      
      // Set up the response function for this connection
      rpc.SetRpcResponseFunction([this](const void* data, uint32_t len) {
        SendRpcChunk(conn_, data, len);
      });
      
      // Signal that initialization is complete
      {
        std::lock_guard<std::mutex> lock(init_mutex_);
        task_runner_ = &task_runner;
        rpc_ = &rpc;
        initialized_ = true;
        init_cv_.notify_one();
      }
      
      // Run the event loop
      task_runner.Run();
    });
  }
  
  ~WebSocketRpcThread() {
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
    std::unique_lock<std::mutex> lock(init_mutex_);
    init_cv_.wait(lock, [this] { return initialized_; });
    
    if (task_runner_ && rpc_) {
      task_runner_->PostTask([this, data = std::vector<uint8_t>(msg.data.begin(), msg.data.end())]() {
        rpc_->OnRpcRequest(data.data(), static_cast<uint32_t>(data.size()));
      });
    }
  }
  Rpc* rpc_ = nullptr;
  
 private:
  std::thread rpc_thread_;            // Dedicated thread
  base::HttpServerConnection* conn_;  // WebSocket connection
  
  // These are valid only in the worker thread context
  base::UnixTaskRunner* task_runner_ = nullptr;
  
  // Synchronization
  std::mutex init_mutex_;
  std::condition_variable init_cv_;
  bool initialized_ = false;
};

class Httpd : public base::HttpRequestHandler {
 public:
  explicit Httpd(std::unique_ptr<TraceProcessor>, int timeout_seconds);
  ~Httpd() override;
  void Run(const std::string& host, int port);

 private:
  // HttpRequestHandler implementation.
  void HandleFileUpload(const base::HttpRequest& req);  
  void OnHttpRequest(const base::HttpRequest&) override;
  void OnWebsocketMessage(const base::WebsocketMessage&) override;

  void cleanUpInactiveInstances();
  static void ServeHelpPage(const base::HttpRequest&);

  Rpc global_trace_processor_rpc_;
  std::unordered_map<base::HttpServerConnection*, std::unique_ptr<WebSocketRpcThread>> websocket_rpc_threads_;

  int timeout_seconds_;
  base::UnixTaskRunner task_runner_;
  base::HttpServer http_srv_;
  std::mutex websocket_rpc_mutex_;

  void OnHttpConnectionClosed(base::HttpServerConnection* conn) override {
    std::lock_guard<std::mutex> lock(websocket_rpc_mutex_);
    auto it = websocket_rpc_threads_.find(conn);
    if (it != websocket_rpc_threads_.end()) {
      websocket_rpc_threads_.erase(it);
    }
  }
};

base::StringView Vec2Sv(const std::vector<uint8_t>& v) {
  return {reinterpret_cast<const char*>(v.data()), v.size()};
}


Httpd::Httpd(std::unique_ptr<TraceProcessor> preloaded_instance, int timeout_seconds)
    : global_trace_processor_rpc_(std::move(preloaded_instance)),
      timeout_seconds_(timeout_seconds),
      http_srv_(&task_runner_, this) {}
Httpd::~Httpd() = default;


void Httpd::Run(const std::string& host, int port) {
  PERFETTO_ILOG("[HTTP] Starting RPC server on %s:%d", host.c_str(), port);
  PERFETTO_LOG(
      "[HTTP] This server can be used by reloading https://ui.perfetto.dev and "
      "clicking on YES on the \"Trace Processor native acceleration\" dialog "
      "or through the Python API (see "
      "https://perfetto.dev/docs/analysis/trace-processor#python-api).");

  for (const auto& kAllowedCORSOrigin : kAllowedCORSOrigins) {
    http_srv_.AddAllowedOrigin(kAllowedCORSOrigin);
  }
  http_srv_.Start(host, port);
  // Declare as std::function first
  std::function<void()> cleanup_task;
  cleanup_task = [this, &cleanup_task]() {
    cleanUpInactiveInstances();
    task_runner_.PostDelayedTask(cleanup_task, static_cast<uint32_t>(timeout_seconds_ * 1000));
  };
  
  // Initial scheduling
  task_runner_.PostDelayedTask(cleanup_task, static_cast<uint32_t>(timeout_seconds_ * 1000));

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

  if (req.uri == "/getFileList") {
    std::vector<uint8_t> file_names;
    
    // Check if storage directory exists
    if (std::filesystem::exists(kStorageDir) && 
        std::filesystem::is_directory(kStorageDir)) {
      std::error_code ec;
      for (const auto& entry : std::filesystem::directory_iterator(kStorageDir, ec)) {
        if (!ec && entry.is_regular_file()) {
          std::string filename = entry.path().filename().string();
          file_names.insert(file_names.end(), filename.begin(), filename.end());
          file_names.push_back('\n');
        }
      }
      if (ec) {
        PERFETTO_PLOG("Error reading storage directory: %s", ec.message().c_str());
      }
      // Remove trailing newline if any files were found
      if (!file_names.empty()) {
        file_names.pop_back();
      }
    }
    
    // Return empty list if directory doesn't exist or is empty
    return conn.SendResponse("200 OK", default_headers, Vec2Sv(file_names));
  }

  if(req.uri == "/upload"){
    HandleFileUpload(req);
    return;
  }

  if (req.uri == "/status") {
    auto status = global_trace_processor_rpc_.GetStatus();
    return conn.SendResponse("200 OK", default_headers, Vec2Sv(status));
  }

  if (req.uri == "/websocket" && req.is_websocket_handshake) {
    // Will trigger OnWebsocketMessage() when is received.
    // It returns a 403 if the origin is not in kAllowedCORSOrigins.
    return conn.UpgradeToWebsocket(req);
  }

  // --- Everything below this line is a legacy endpoint not used by the UI.
  // There are two generations of pre-websocket legacy-ness:
  // 1. The /rpc based endpoint. This is based on a chunked transfer, doing one
  //    POST request for each RPC invocation. All RPC methods are multiplexed
  //    into this one. This is still used by the python API.
  // 2. The REST API, with one enpoint per RPC method (/parse, /query, ...).
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
  // The batch size is determined by |cells_per_batch_| and
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

void Httpd::HandleFileUpload(const base::HttpRequest& req) {
  // Create storage directory if it doesn't exist
  if (mkdir(kStorageDir, 0755) && errno != EEXIST) {
    PERFETTO_PLOG("Failed to create storage directory");
    return req.conn->SendResponseAndClose("500 Internal Server Error");
  }

  // Get filename from custom header
  base::StringView filename_hdr = req.GetHeader("x-filename").value();
  if (filename_hdr.empty()) {
    return req.conn->SendResponseAndClose("400 Bad Request", {}, 
        "Missing X-Filename header");
  }
  std::string filename = filename_hdr.ToStdString();

  // Write raw body to file
  std::string filepath = std::string(kStorageDir) + "/" + filename;
  std::ofstream out(filepath, std::ios::binary);
  if (!out) {
    PERFETTO_PLOG("Failed to create %s", filepath.c_str());
    return req.conn->SendResponseAndClose("500 Internal Server Error");
  }

 out.write(req.body.data(), static_cast<std::streamsize>(req.body.size()));
  if (!out) {
    PERFETTO_PLOG("Failed to write to %s", filepath.c_str());
    return req.conn->SendResponseAndClose("500 Internal Server Error");
  }

  // Send success response
  req.conn->SendResponse("200 OK", {
      "Content-Type: application/json"
  }, base::StringView("{\"status\":\"success\",\"path\":\"" + filepath + "\"}"));
}

void Httpd::OnWebsocketMessage(const base::WebsocketMessage& msg) {
    // Check if this connection already has a dedicated thread
    std::lock_guard<std::mutex> lock(websocket_rpc_mutex_);
    
    auto it = websocket_rpc_threads_.find(msg.conn);
    if (it == websocket_rpc_threads_.end()) {
        // Create new thread for this connection
        auto new_thread = std::make_unique<WebSocketRpcThread>(msg.conn);
        auto result = websocket_rpc_threads_.emplace(msg.conn, std::move(new_thread));
        it = result.first;
    }
    
    // Dispatch to the dedicated thread
    it->second->OnWebsocketMessage(msg);
}

}  // namespace

void RunHttpRPCServer(std::unique_ptr<TraceProcessor> preloaded_instance,
                      const std::string& host_address, const std::string& port_number, int timeout_seconds) {
  Httpd srv(std::move(preloaded_instance), timeout_seconds);
  std::optional<int> port_opt = base::StringToInt32(port_number);
  int port = port_opt.has_value() ? *port_opt : kBindPort;
  srv.Run(host_address, port);
}

void Httpd::cleanUpInactiveInstances() {
  uint64_t kInactivityNs = static_cast<uint64_t>(timeout_seconds_) * 1000000000ULL;
  uint64_t now = static_cast<uint64_t>(base::GetWallTimeNs().count());
  auto it = websocket_rpc_threads_.begin();
  while (it != websocket_rpc_threads_.end()) {
    if (it->first && now - it->second->rpc_->last_accessed_ns > kInactivityNs) {
      it = websocket_rpc_threads_.erase(it);
      PERFETTO_ILOG("Cleaned up inactive websocket connection");
      } else {
      it++;
    }
  }
}

void Httpd::ServeHelpPage(const base::HttpRequest& req) {
  static const char kPage[] = R"(Perfetto Trace Processor RPC Server


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
