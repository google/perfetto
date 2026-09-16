/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <emscripten/emscripten.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

#include "perfetto/base/compiler.h"
#include "src/trace_processor/rpc/rpc.h"

namespace perfetto::trace_processor {

namespace {
using RpcResponseFn = void(const void*, uint32_t);

Rpc* g_trace_processor_rpc;
Rpc::Stream* g_rpc_stream;
Rpc::RequestHandle* g_pending_request;

uint32_t g_max_write_size;

PERFETTO_NO_INLINE void OutOfMemoryHandler() {
  fprintf(stderr, "\nCannot enlarge memory\n");
  abort();
}

}  // namespace

// +---------------------------------------------------------------------------+
// | Exported functions called by the JS/TS running in the worker.             |
// +---------------------------------------------------------------------------+
extern "C" {

// Returns the address JS must write the first request into.
uint8_t* EMSCRIPTEN_KEEPALIVE
trace_processor_rpc_init(RpcResponseFn* RpcResponseFn, uint32_t);
uint8_t* trace_processor_rpc_init(RpcResponseFn* resp_function,
                                  uint32_t max_write_size) {
  // Usually OOMs manifest as a failure in dlmalloc() -> sbrk() ->
  //_emscripten_resize_heap() which aborts itself. However in some rare cases
  // sbrk() can fail outside of _emscripten_resize_heap and just return null.
  // When that happens, just abort with the same message that
  // _emscripten_resize_heap uses, so error_dialog.ts shows a OOM message.
  std::set_new_handler(&OutOfMemoryHandler);

  g_trace_processor_rpc = new Rpc();

  // |resp_function| is a JS-bound function passed by wasm_bridge.ts. It will
  // call back into JavaScript. There the JS code will copy the passed
  // buffer with the response (a proto-encoded TraceProcessorRpc message) and
  // postMessage() it to the controller. See the comment in wasm_bridge.ts for
  // an overview of the JS<>Wasm callstack.
  g_rpc_stream = new Rpc::Stream(*g_trace_processor_rpc, resp_function);

  g_max_write_size = max_write_size;
  g_pending_request =
      new Rpc::RequestHandle(g_rpc_stream->BeginRequest(g_max_write_size));
  return g_pending_request->data();
}

// Returning the next address rather than taking one keeps this to a single
// JS->Wasm call per request: JS knows where to write before it knows the size.
uint8_t* EMSCRIPTEN_KEEPALIVE trace_processor_on_rpc_request(uint32_t);
uint8_t* trace_processor_on_rpc_request(uint32_t size) {
  g_pending_request->EndRequest(size);
  *g_pending_request = g_rpc_stream->BeginRequest(g_max_write_size);
  return g_pending_request->data();
}

}  // extern "C"
}  // namespace perfetto::trace_processor

int main(int, char**) {
  // This is unused but is needed for the following reasons:
  // - We need the callMain() Emscripten JS helper function for traceconv (but
  //   not for trace_processor).
  // - Newer versions of emscripten require that callMain is explicitly exported
  //   via EXPORTED_RUNTIME_METHODS = ['callMain'].
  // - We have one set of EXPORTED_RUNTIME_METHODS for both
  //   trace_processor.wasm (which does not need a main()) and traceconv (which
  //   does).
  // - Without this main(), the Wasm bootstrap code will cause a JS error at
  //   runtime when trying to load trace_processor.js.
  return 0;
}
