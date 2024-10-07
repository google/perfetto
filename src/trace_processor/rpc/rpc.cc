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

#include "src/trace_processor/rpc/rpc.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/version.h"
#include "perfetto/ext/protozero/proto_ring_buffer.h"
#include "perfetto/ext/trace_processor/rpc/query_result_serializer.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/metatrace_config.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/tp_metatrace.h"
#include "src/trace_processor/util/status_macros.h"

#include "protos/perfetto/trace_processor/metatrace_categories.pbzero.h"
#include "protos/perfetto/trace_processor/trace_processor.pbzero.h"

namespace perfetto::trace_processor {

namespace {
// Writes a "Loading trace ..." update every N bytes.
constexpr size_t kProgressUpdateBytes = 50ul * 1000 * 1000;
using TraceProcessorRpcStream = protos::pbzero::TraceProcessorRpcStream;
using RpcProto = protos::pbzero::TraceProcessorRpc;

// Most RPC messages are either very small or a query results.
// QueryResultSerializer splits rows into batches of approximately 128KB. Try
// avoid extra heap allocations for the nominal case.
constexpr auto kSliceSize =
    QueryResultSerializer::kDefaultBatchSplitThreshold + 4096;

// Holds a trace_processor::TraceProcessorRpc pbzero message. Avoids extra
// copies by doing direct scattered calls from the fragmented heap buffer onto
// the RpcResponseFunction (the receiver is expected to deal with arbitrary
// fragmentation anyways). It also takes care of prefixing each message with
// the proto preamble and varint size.
class Response {
 public:
  Response(int64_t seq, int method);
  Response(const Response&) = delete;
  Response& operator=(const Response&) = delete;
  RpcProto* operator->() { return msg_; }
  void Send(Rpc::RpcResponseFunction);

 private:
  RpcProto* msg_ = nullptr;

  // The reason why we use TraceProcessorRpcStream as root message is because
  // the RPC wire protocol expects each message to be prefixed with a proto
  // preamble and varint size. This happens to be the same serialization of a
  // repeated field (this is really the same trick we use between
  // Trace and TracePacket in trace.proto)
  protozero::HeapBuffered<TraceProcessorRpcStream> buf_;
};

Response::Response(int64_t seq, int method) : buf_(kSliceSize, kSliceSize) {
  msg_ = buf_->add_msg();
  msg_->set_seq(seq);
  msg_->set_response(static_cast<RpcProto::TraceProcessorMethod>(method));
}

void Response::Send(Rpc::RpcResponseFunction send_fn) {
  buf_->Finalize();
  for (const auto& slice : buf_.GetSlices()) {
    auto range = slice.GetUsedRange();
    send_fn(range.begin, static_cast<uint32_t>(range.size()));
  }
}

}  // namespace

Rpc::Rpc(std::unique_ptr<TraceProcessor> preloaded_instance)
    : trace_processor_(std::move(preloaded_instance)) {
  if (!trace_processor_)
    ResetTraceProcessorInternal(Config());
}

Rpc::Rpc() : Rpc(nullptr) {}
Rpc::~Rpc() = default;

void Rpc::ResetTraceProcessorInternal(const Config& config) {
  trace_processor_config_ = config;
  trace_processor_ = TraceProcessor::CreateInstance(config);
  bytes_parsed_ = bytes_last_progress_ = 0;
  t_parse_started_ = base::GetWallTimeNs().count();
  // Deliberately not resetting the RPC channel state (rxbuf_, {tx,rx}_seq_id_).
  // This is invoked from the same client to clear the current trace state
  // before loading a new one. The IPC channel is orthogonal to that and the
  // message numbering continues regardless of the reset.
}

void Rpc::OnRpcRequest(const void* data, size_t len) {
  rxbuf_.Append(data, len);
  for (;;) {
    auto msg = rxbuf_.ReadMessage();
    if (!msg.valid()) {
      if (msg.fatal_framing_error) {
        protozero::HeapBuffered<TraceProcessorRpcStream> err_msg;
        err_msg->add_msg()->set_fatal_error("RPC framing error");
        auto err = err_msg.SerializeAsArray();
        rpc_response_fn_(err.data(), static_cast<uint32_t>(err.size()));
        rpc_response_fn_(nullptr, 0);  // Disconnect.
      }
      break;
    }
    ParseRpcRequest(msg.start, msg.len);
  }
}

namespace {

using ProtoEnum = protos::pbzero::MetatraceCategories;
TraceProcessor::MetatraceCategories MetatraceCategoriesToPublicEnum(
    ProtoEnum categories) {
  TraceProcessor::MetatraceCategories result =
      TraceProcessor::MetatraceCategories::NONE;
  if (categories & ProtoEnum::QUERY_TIMELINE) {
    result = static_cast<TraceProcessor::MetatraceCategories>(
        result | TraceProcessor::MetatraceCategories::QUERY_TIMELINE);
  }
  if (categories & ProtoEnum::QUERY_DETAILED) {
    result = static_cast<TraceProcessor::MetatraceCategories>(
        result | TraceProcessor::MetatraceCategories::QUERY_DETAILED);
  }
  if (categories & ProtoEnum::FUNCTION_CALL) {
    result = static_cast<TraceProcessor::MetatraceCategories>(
        result | TraceProcessor::MetatraceCategories::FUNCTION_CALL);
  }
  if (categories & ProtoEnum::DB) {
    result = static_cast<TraceProcessor::MetatraceCategories>(
        result | TraceProcessor::MetatraceCategories::DB);
  }
  if (categories & ProtoEnum::API_TIMELINE) {
    result = static_cast<TraceProcessor::MetatraceCategories>(
        result | TraceProcessor::MetatraceCategories::API_TIMELINE);
  }
  return result;
}

}  // namespace

// [data, len] here is a tokenized TraceProcessorRpc proto message, without the
// size header.
void Rpc::ParseRpcRequest(const uint8_t* data, size_t len) {
  RpcProto::Decoder req(data, len);

  // We allow restarting the sequence from 0. This happens when refreshing the
  // browser while using the external trace_processor_shell --httpd.
  if (req.seq() != 0 && rx_seq_id_ != 0 && req.seq() != rx_seq_id_ + 1) {
    char err_str[255];
    // "(ERR:rpc_seq)" is intercepted by error_dialog.ts in the UI.
    snprintf(err_str, sizeof(err_str),
             "RPC request out of order. Expected %" PRId64 ", got %" PRId64
             " (ERR:rpc_seq)",
             rx_seq_id_ + 1, req.seq());
    PERFETTO_ELOG("%s", err_str);
    protozero::HeapBuffered<TraceProcessorRpcStream> err_msg;
    err_msg->add_msg()->set_fatal_error(err_str);
    auto err = err_msg.SerializeAsArray();
    rpc_response_fn_(err.data(), static_cast<uint32_t>(err.size()));
    rpc_response_fn_(nullptr, 0);  // Disconnect.
    return;
  }
  rx_seq_id_ = req.seq();

  // The static cast is to prevent that the compiler breaks future proofness.
  const int req_type = static_cast<int>(req.request());
  static const char kErrFieldNotSet[] = "RPC error: request field not set";
  switch (req_type) {
    case RpcProto::TPM_APPEND_TRACE_DATA: {
      Response resp(tx_seq_id_++, req_type);
      auto* result = resp->set_append_result();
      if (!req.has_append_trace_data()) {
        result->set_error(kErrFieldNotSet);
      } else {
        protozero::ConstBytes byte_range = req.append_trace_data();
        base::Status res = Parse(byte_range.data, byte_range.size);
        if (!res.ok()) {
          result->set_error(res.message());
        }
      }
      resp.Send(rpc_response_fn_);
      break;
    }
    case RpcProto::TPM_FINALIZE_TRACE_DATA: {
      Response resp(tx_seq_id_++, req_type);
      NotifyEndOfFile();
      resp.Send(rpc_response_fn_);
      break;
    }
    case RpcProto::TPM_QUERY_STREAMING: {
      if (!req.has_query_args()) {
        Response resp(tx_seq_id_++, req_type);
        auto* result = resp->set_query_result();
        result->set_error(kErrFieldNotSet);
        resp.Send(rpc_response_fn_);
      } else {
        protozero::ConstBytes args = req.query_args();
        protos::pbzero::QueryArgs::Decoder query(args.data, args.size);
        std::string sql = query.sql_query().ToStdString();

        PERFETTO_TP_TRACE(metatrace::Category::API_TIMELINE, "RPC_QUERY",
                          [&](metatrace::Record* r) {
                            r->AddArg("SQL", sql);
                            if (query.has_tag()) {
                              r->AddArg("tag", query.tag());
                            }
                          });

        auto it = trace_processor_->ExecuteQuery(sql);
        QueryResultSerializer serializer(std::move(it));
        for (bool has_more = true; has_more;) {
          const auto seq_id = tx_seq_id_++;
          Response resp(seq_id, req_type);
          has_more = serializer.Serialize(resp->set_query_result());
          const uint32_t resp_size = resp->Finalize();
          if (resp_size < protozero::proto_utils::kMaxMessageLength) {
            // This is the nominal case.
            resp.Send(rpc_response_fn_);
            continue;
          }
          // In rare cases a query can end up with a batch which is too big.
          // Normally batches are automatically split before hitting the limit,
          // but one can come up with a query where a single cell is > 256MB.
          // If this happens, just bail out gracefully rather than creating an
          // unparsable proto which will cause a RPC framing error.
          // If we hit this, we have to discard `resp` because it's
          // unavoidably broken (due to have overflown the 4-bytes size) and
          // can't be parsed. Instead create a new response with the error.
          Response err_resp(seq_id, req_type);
          auto* qres = err_resp->set_query_result();
          qres->add_batch()->set_is_last_batch(true);
          qres->set_error(
              "The query ended up with a response that is too big (" +
              std::to_string(resp_size) +
              " bytes). This usually happens when a single row is >= 256 MiB. "
              "See also WRITE_FILE for dealing with large rows.");
          err_resp.Send(rpc_response_fn_);
          break;
        }
      }
      break;
    }
    case RpcProto::TPM_COMPUTE_METRIC: {
      Response resp(tx_seq_id_++, req_type);
      auto* result = resp->set_metric_result();
      if (!req.has_compute_metric_args()) {
        result->set_error(kErrFieldNotSet);
      } else {
        protozero::ConstBytes args = req.compute_metric_args();
        ComputeMetricInternal(args.data, args.size, result);
      }
      resp.Send(rpc_response_fn_);
      break;
    }
    case RpcProto::TPM_GET_METRIC_DESCRIPTORS: {
      Response resp(tx_seq_id_++, req_type);
      auto descriptor_set = trace_processor_->GetMetricDescriptors();
      auto* result = resp->set_metric_descriptors();
      result->AppendRawProtoBytes(descriptor_set.data(), descriptor_set.size());
      resp.Send(rpc_response_fn_);
      break;
    }
    case RpcProto::TPM_RESTORE_INITIAL_TABLES: {
      trace_processor_->RestoreInitialTables();
      Response resp(tx_seq_id_++, req_type);
      resp.Send(rpc_response_fn_);
      break;
    }
    case RpcProto::TPM_ENABLE_METATRACE: {
      using protos::pbzero::MetatraceCategories;
      protozero::ConstBytes args = req.enable_metatrace_args();
      EnableMetatrace(args.data, args.size);

      Response resp(tx_seq_id_++, req_type);
      resp.Send(rpc_response_fn_);
      break;
    }
    case RpcProto::TPM_DISABLE_AND_READ_METATRACE: {
      Response resp(tx_seq_id_++, req_type);
      DisableAndReadMetatraceInternal(resp->set_metatrace());
      resp.Send(rpc_response_fn_);
      break;
    }
    case RpcProto::TPM_GET_STATUS: {
      Response resp(tx_seq_id_++, req_type);
      std::vector<uint8_t> status = GetStatus();
      resp->set_status()->AppendRawProtoBytes(status.data(), status.size());
      resp.Send(rpc_response_fn_);
      break;
    }
    case RpcProto::TPM_RESET_TRACE_PROCESSOR: {
      Response resp(tx_seq_id_++, req_type);
      protozero::ConstBytes args = req.reset_trace_processor_args();
      ResetTraceProcessor(args.data, args.size);
      resp.Send(rpc_response_fn_);
      break;
    }
    case RpcProto::TPM_REGISTER_SQL_PACKAGE: {
      Response resp(tx_seq_id_++, req_type);
      base::Status status = RegisterSqlPackage(req.register_sql_package_args());
      auto* res = resp->set_register_sql_package_result();
      if (!status.ok()) {
        res->set_error(status.message());
      }
      resp.Send(rpc_response_fn_);
      break;
    }
    default: {
      // This can legitimately happen if the client is newer. We reply with a
      // generic "unkown request" response, so the client can do feature
      // detection
      PERFETTO_DLOG("[RPC] Uknown request type (%d), size=%zu", req_type, len);
      Response resp(tx_seq_id_++, req_type);
      resp->set_invalid_request(
          static_cast<RpcProto::TraceProcessorMethod>(req_type));
      resp.Send(rpc_response_fn_);
      break;
    }
  }  // switch(req_type)
}

base::Status Rpc::Parse(const uint8_t* data, size_t len) {
  PERFETTO_TP_TRACE(
      metatrace::Category::API_TIMELINE, "RPC_PARSE",
      [&](metatrace::Record* r) { r->AddArg("length", std::to_string(len)); });
  if (eof_) {
    // Reset the trace processor state if another trace has been previously
    // loaded. Use the same TraceProcessor Config.
    ResetTraceProcessorInternal(trace_processor_config_);
  }

  eof_ = false;
  bytes_parsed_ += len;
  MaybePrintProgress();

  if (len == 0)
    return base::OkStatus();

  // TraceProcessor needs take ownership of the memory chunk.
  std::unique_ptr<uint8_t[]> data_copy(new uint8_t[len]);
  memcpy(data_copy.get(), data, len);
  return trace_processor_->Parse(std::move(data_copy), len);
}

base::Status Rpc::NotifyEndOfFile() {
  PERFETTO_TP_TRACE(metatrace::Category::API_TIMELINE,
                    "RPC_NOTIFY_END_OF_FILE");

  eof_ = true;
  RETURN_IF_ERROR(trace_processor_->NotifyEndOfFile());
  MaybePrintProgress();
  return base::OkStatus();
}

void Rpc::ResetTraceProcessor(const uint8_t* args, size_t len) {
  protos::pbzero::ResetTraceProcessorArgs::Decoder reset_trace_processor_args(
      args, len);
  Config config;
  if (reset_trace_processor_args.has_drop_track_event_data_before()) {
    config.drop_track_event_data_before =
        reset_trace_processor_args.drop_track_event_data_before() ==
                protos::pbzero::ResetTraceProcessorArgs::
                    TRACK_EVENT_RANGE_OF_INTEREST
            ? DropTrackEventDataBefore::kTrackEventRangeOfInterest
            : DropTrackEventDataBefore::kNoDrop;
  }
  if (reset_trace_processor_args.has_ingest_ftrace_in_raw_table()) {
    config.ingest_ftrace_in_raw_table =
        reset_trace_processor_args.ingest_ftrace_in_raw_table();
  }
  if (reset_trace_processor_args.has_analyze_trace_proto_content()) {
    config.analyze_trace_proto_content =
        reset_trace_processor_args.analyze_trace_proto_content();
  }
  if (reset_trace_processor_args.has_ftrace_drop_until_all_cpus_valid()) {
    config.soft_drop_ftrace_data_before =
        reset_trace_processor_args.ftrace_drop_until_all_cpus_valid()
            ? SoftDropFtraceDataBefore::kAllPerCpuBuffersValid
            : SoftDropFtraceDataBefore::kNoDrop;
  }
  ResetTraceProcessorInternal(config);
}

base::Status Rpc::RegisterSqlPackage(protozero::ConstBytes bytes) {
  protos::pbzero::RegisterSqlPackageArgs::Decoder args(bytes);
  SqlPackage package;
  package.name = args.package_name().ToStdString();
  package.allow_override = args.allow_override();
  for (auto it = args.modules(); it; ++it) {
    protos::pbzero::RegisterSqlPackageArgs::Module::Decoder m(*it);
    package.modules.emplace_back(m.name().ToStdString(), m.sql().ToStdString());
  }
  return trace_processor_->RegisterSqlPackage(package);
}

void Rpc::MaybePrintProgress() {
  if (eof_ || bytes_parsed_ - bytes_last_progress_ > kProgressUpdateBytes) {
    bytes_last_progress_ = bytes_parsed_;
    auto t_load_s =
        static_cast<double>(base::GetWallTimeNs().count() - t_parse_started_) /
        1e9;
    fprintf(stderr, "\rLoading trace %.2f MB (%.1f MB/s)%s",
            static_cast<double>(bytes_parsed_) / 1e6,
            static_cast<double>(bytes_parsed_) / 1e6 / t_load_s,
            (eof_ ? "\n" : ""));
    fflush(stderr);
  }
}

void Rpc::Query(const uint8_t* args,
                size_t len,
                const QueryResultBatchCallback& result_callback) {
  protos::pbzero::QueryArgs::Decoder query(args, len);
  std::string sql = query.sql_query().ToStdString();
  PERFETTO_TP_TRACE(metatrace::Category::API_TIMELINE, "RPC_QUERY",
                    [&](metatrace::Record* r) {
                      r->AddArg("SQL", sql);
                      if (query.has_tag()) {
                        r->AddArg("tag", query.tag());
                      }
                    });

  auto it = trace_processor_->ExecuteQuery(sql);

  QueryResultSerializer serializer(std::move(it));

  std::vector<uint8_t> res;
  for (bool has_more = true; has_more;) {
    has_more = serializer.Serialize(&res);
    result_callback(res.data(), res.size(), has_more);
    res.clear();
  }
}

void Rpc::RestoreInitialTables() {
  trace_processor_->RestoreInitialTables();
}

std::vector<uint8_t> Rpc::ComputeMetric(const uint8_t* args, size_t len) {
  protozero::HeapBuffered<protos::pbzero::ComputeMetricResult> result;
  ComputeMetricInternal(args, len, result.get());
  return result.SerializeAsArray();
}

void Rpc::ComputeMetricInternal(const uint8_t* data,
                                size_t len,
                                protos::pbzero::ComputeMetricResult* result) {
  protos::pbzero::ComputeMetricArgs::Decoder args(data, len);
  std::vector<std::string> metric_names;
  for (auto it = args.metric_names(); it; ++it) {
    metric_names.emplace_back(it->as_std_string());
  }

  PERFETTO_TP_TRACE(metatrace::Category::API_TIMELINE, "RPC_COMPUTE_METRIC",
                    [&](metatrace::Record* r) {
                      for (const auto& metric : metric_names) {
                        r->AddArg("Metric", metric);
                        r->AddArg("Format", std::to_string(args.format()));
                      }
                    });

  PERFETTO_DLOG("[RPC] ComputeMetrics(%zu, %s), format=%d", metric_names.size(),
                metric_names.empty() ? "" : metric_names.front().c_str(),
                args.format());
  switch (args.format()) {
    case protos::pbzero::ComputeMetricArgs::BINARY_PROTOBUF: {
      std::vector<uint8_t> metrics_proto;
      base::Status status =
          trace_processor_->ComputeMetric(metric_names, &metrics_proto);
      if (status.ok()) {
        result->set_metrics(metrics_proto.data(), metrics_proto.size());
      } else {
        result->set_error(status.message());
      }
      break;
    }
    case protos::pbzero::ComputeMetricArgs::TEXTPROTO: {
      std::string metrics_string;
      base::Status status = trace_processor_->ComputeMetricText(
          metric_names, TraceProcessor::MetricResultFormat::kProtoText,
          &metrics_string);
      if (status.ok()) {
        result->set_metrics_as_prototext(metrics_string);
      } else {
        result->set_error(status.message());
      }
      break;
    }
    case protos::pbzero::ComputeMetricArgs::JSON: {
      std::string metrics_string;
      base::Status status = trace_processor_->ComputeMetricText(
          metric_names, TraceProcessor::MetricResultFormat::kJson,
          &metrics_string);
      if (status.ok()) {
        result->set_metrics_as_json(metrics_string);
      } else {
        result->set_error(status.message());
      }
      break;
    }
  }
}

void Rpc::EnableMetatrace(const uint8_t* data, size_t len) {
  using protos::pbzero::MetatraceCategories;
  TraceProcessor::MetatraceConfig config;
  protos::pbzero::EnableMetatraceArgs::Decoder args(data, len);
  config.categories = MetatraceCategoriesToPublicEnum(
      static_cast<MetatraceCategories>(args.categories()));
  trace_processor_->EnableMetatrace(config);
}

std::vector<uint8_t> Rpc::DisableAndReadMetatrace() {
  protozero::HeapBuffered<protos::pbzero::DisableAndReadMetatraceResult> result;
  DisableAndReadMetatraceInternal(result.get());
  return result.SerializeAsArray();
}

void Rpc::DisableAndReadMetatraceInternal(
    protos::pbzero::DisableAndReadMetatraceResult* result) {
  std::vector<uint8_t> trace_proto;
  base::Status status = trace_processor_->DisableAndReadMetatrace(&trace_proto);
  if (status.ok()) {
    result->set_metatrace(trace_proto.data(), trace_proto.size());
  } else {
    result->set_error(status.message());
  }
}

std::vector<uint8_t> Rpc::GetStatus() {
  protozero::HeapBuffered<protos::pbzero::StatusResult> status;
  status->set_loaded_trace_name(trace_processor_->GetCurrentTraceName());
  status->set_human_readable_version(base::GetVersionString());
  if (const char* version_code = base::GetVersionCode(); version_code) {
    status->set_version_code(version_code);
  }
  status->set_api_version(protos::pbzero::TRACE_PROCESSOR_CURRENT_API_VERSION);
  return status.SerializeAsArray();
}

}  // namespace perfetto::trace_processor
