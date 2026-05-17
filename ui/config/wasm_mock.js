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

// Stub for `ui/src/wasm/*` imports under jest. Vite aliases these to the
// emcc-emitted glue in `ui/src/gen/*` at build time; jest doesn't run that
// alias, and no unit test exercises the wasm runtime. A factory that throws
// makes accidental invocations obvious.
const factory = () => {
  throw new Error('wasm runtime is not available in jest');
};
module.exports = {default: factory, __esModule: true};
