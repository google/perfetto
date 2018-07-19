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

import {createEmptyState, State} from '../common/state';

const state: State = createEmptyState();

function main() {
  // TODO(hjd): Compile this with the worker lib.
  // tslint:disable-next-line no-any
  (self as any).onmessage = (_: MessageEvent) => {
    state.i++;
    // TODO(hjd): Compile this with the worker lib.
    // tslint:disable-next-line no-any
    (self as any).postMessage(state);
  };
}

main();
