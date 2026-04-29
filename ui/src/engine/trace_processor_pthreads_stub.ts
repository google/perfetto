// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Placeholder import target for the pthreads-enabled trace_processor wasm
// module. In production builds the rollup replace plugin rewrites this
// import to '../gen/trace_processor_pthreads' so the real module is
// linked. In configurations that don't ship the pthreads variant
// (e.g. --only-wasm-memory64) the stub stays in place; the runtime
// selector in wasm_bridge.ts must never reach this default export
// because hasPthreadsSupport() returns false outside cross-origin
// isolated contexts.
export default (() => {
  throw new Error(
    'Unable to load the pthreads trace_processor.wasm. ' +
      'This build did not include the pthreads variant; check that ' +
      '--only-wasm-memory64 was not passed to ui/build, and that the ' +
      'host page is cross-origin isolated (COOP+COEP).',
  );
}) as never;
