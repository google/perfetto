// Copyright (C) 2025 The Android Open Source Project
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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {Flow} from '../../core/flow_types';
import {
  LYNX_BACKGROUND_THREAD_NAME,
  NATIVEMODULE_CALL,
  NATIVEMODULE_NETWORK_REQUEST,
} from '../../lynx_perf/constants';

export function isSyncNativeModule(flows: Flow[]) {
  for (const flow of flows) {
    if (!flow.begin.threadName.includes(LYNX_BACKGROUND_THREAD_NAME)) {
      return false;
    }
  }
  return true;
}

export function isNativeModuleCall(name: string) {
  return name === NATIVEMODULE_CALL || name === NATIVEMODULE_NETWORK_REQUEST;
}
