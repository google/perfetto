// Copyright (C) 2018 The Android Open Source Project
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

export default (() => {
  throw new Error(
    'Unable to load the 32-bit trace_processor.wasm. ' +
      'This is because you are running in a browser that does NOT support ' +
      'Memory64 but passed --only-wasm-memory64 to ui/build ' +
      '(run-dev-server does that)',
  );
}) as never;
