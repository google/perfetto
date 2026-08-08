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
#if defined(__EMSCRIPTEN_PTHREADS__)
#include <emscripten/threading.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "perfetto/base/compiler.h"
#include "src/trace_processor/rpc/rpc.h"

namespace perfetto::trace_processor {

namespace {
using RpcResponseFn = void(const void*, uint32_t);

Rpc* g_trace_processor_rpc;
RpcResponseFn* g_response_fn;

#if defined(__EMSCRIPTEN_PTHREADS__)
struct PendingReply {
  std::vector<uint8_t> data;
};

void SendReplyOnFrontend(int, PendingReply* reply) {
  g_response_fn(reply->data.data(), static_cast<uint32_t>(reply->data.size()));
  delete reply;
}

void SendReply(const void* data, uint32_t size) {
  if (emscripten_is_main_runtime_thread()) {
    g_response_fn(data, size);
    return;
  }
  auto* reply = new PendingReply;
  reply->data.resize(size);
  if (size != 0)
    memcpy(reply->data.data(), data, size);
  emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_VIP,
                                              &SendReplyOnFrontend, 0, reply);
}
#endif

PERFETTO_NO_INLINE void OutOfMemoryHandler() {
  fprintf(stderr, "\nCannot enlarge memory\n");
  abort();
}

}  // namespace

// +---------------------------------------------------------------------------+
// | Exported functions called by the JS/TS running in the worker.             |
// +---------------------------------------------------------------------------+
extern "C" {

void EMSCRIPTEN_KEEPALIVE trace_processor_rpc_init(RpcResponseFn*);
void trace_processor_rpc_init(RpcResponseFn* resp_function) {
  // Usually OOMs manifest as a failure in dlmalloc() -> sbrk() ->
  //_emscripten_resize_heap() which aborts itself. However in some rare cases
  // sbrk() can fail outside of _emscripten_resize_heap and just return null.
  // When that happens, just abort with the same message that
  // _emscripten_resize_heap uses, so error_dialog.ts shows a OOM message.
  std::set_new_handler(&OutOfMemoryHandler);

  g_trace_processor_rpc = new Rpc();
  g_response_fn = resp_function;

  // |resp_function| is a JS-bound function passed by wasm_bridge.ts. It will
  // call back into JavaScript. There the JS code will copy the passed buffer
  // with the response (a proto-encoded TraceProcessorRpc message) and
  // postMessage() it to the controller. Threaded builds first proxy this
  // callback onto the worker's main Emscripten runtime thread.
#if defined(__EMSCRIPTEN_PTHREADS__)
  g_trace_processor_rpc->SetRpcResponseFunction(&SendReply);
  g_trace_processor_rpc->EnableThreadedExecution();
#else
  g_trace_processor_rpc->SetRpcResponseFunction(resp_function);
#endif
}

uint8_t* EMSCRIPTEN_KEEPALIVE trace_processor_rpc_alloc(uint32_t);
uint8_t* trace_processor_rpc_alloc(uint32_t size) {
  return new uint8_t[size];
}

void EMSCRIPTEN_KEEPALIVE trace_processor_on_rpc_request(uint8_t*, uint32_t);
void trace_processor_on_rpc_request(uint8_t* data, uint32_t size) {
  g_trace_processor_rpc->OnRpcRequest(data, size);
  delete[] data;
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
