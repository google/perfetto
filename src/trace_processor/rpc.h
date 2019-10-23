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

#ifndef SRC_TRACE_PROCESSOR_RPC_H_
#define SRC_TRACE_PROCESSOR_RPC_H_

#include <memory>
#include <vector>

#include <stddef.h>
#include <stdint.h>

#include "perfetto/trace_processor/status.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessor;

// This class handles the binary {,un}marshalling for the Trace Processor RPC
// API (see protos/perfetto/trace_processor/trace_processor.proto).
// This is to deal with cases where the client of the trace processor is not
// some in-process C++ code but a remote process:
// There are two use cases of this:
//   1. The JS<>WASM interop for the web-based UI.
//   2. The HTTP RPC mode of trace_processor_shell that allows the UI to talk
//      to a native trace processor instead of the bundled WASM one.
// This class has (a subset of) the same methods of the public TraceProcessor
// interface, but the methods just take and return proto-encoded binary buffers.
// This class does NOT define how the transport works (e.g. HTTP vs WASM interop
// calls), it just deals with {,un}marshalling.
// This class internally creates and owns a TraceProcessor instance, which
// lifetime is tied to the lifetime of the Rpc instance.
class Rpc {
 public:
  ~Rpc();

  // Loads a trace into the trace processor. Chunked loading is supported for
  // avoid buffering multi-GB traces in memory.
  // Args:
  // (data, len): a protobuf-encoded buffer for of a Trace, or just a portion
  //   of it (if eof=false). In the case of eof=false, the passed buffer does
  //   NOT need to be chunked on TracePacket boundaries. The internals deal
  //   with stitching packets together. This is to allow clients to easily chunk
  //   large trace files with fixed arbitrary chunk sizes.
  // eof: if true, this is the last chunk of the trace. The TraceProcessor
  //   will flush its internal state and reflect all the data ingested until now
  //   into the SQL tables. A further call to LoadTrace() after eof=true will
  //   completely reset the TraceProcessor state and restart from scratch.
  util::Status LoadTrace(const uint8_t* data, size_t len, bool eof = true);

  // Executes a SQL query and returns the results.
  // Args: RawQueryArgs proto-encoded bytes.
  // Returns: RawQueryResult proto-encoded bytes.
  // See protos/perfetto/trace_processor/raw_query.proto for the proto schema.
  // If the query fails the |error| RawQueryResult.field is set accordingly
  std::vector<uint8_t> RawQuery(const uint8_t* args, size_t len);

 private:
  std::unique_ptr<TraceProcessor> trace_processor_;
  bool eof_ = true;  // Reset when calling LoadTrace(..., eof).
  int64_t t_parse_started_ = 0;
  size_t bytes_last_progress_ = 0;
  size_t bytes_parsed_ = 0;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_RPC_H_
