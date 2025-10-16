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
  NATIVEMODULE_CALL,
  NATIVEMODULE_NETWORK_REQUEST,
  DEPRECATED_NATIVEMODULE_CALL,
  NATIVEMODULE_CALLBACK_INVOKE_END,
  NATIVEMODULE_PLATFORM_METHOD_END,
  NATIVEMODULE_INVOKE,
  NATIVEMODULE_CALLBACK,
} from '../../lynx_perf/constants';

export function isSpecialNativeModule(flows: Flow[]) {
  // We validate special native modules according to the following rules:
  // 1. For deprecated timing native module traces, the deprecated callback's end time must be later than the platform method's end time.
  // 2. For non-timing native module traces, the callback's end time must be later than the native module's invoke end time.

  let deprecatedCallbackCallEnd = 0;
  let deprecatedPlatformMethodEnd = 0;
  let nativeModuleInvokeEnd = 0;
  let nativeModuleCallbackEnd = 0;
  for (const flow of flows) {
    if (flow.begin.sliceName === NATIVEMODULE_CALLBACK_INVOKE_END) {
      deprecatedCallbackCallEnd = Number(flow.begin.sliceEndTs);
    }
    if (flow.begin.sliceName === NATIVEMODULE_PLATFORM_METHOD_END) {
      deprecatedPlatformMethodEnd = Number(flow.begin.sliceEndTs);
    }
    if (flow.begin.sliceName === NATIVEMODULE_INVOKE) {
      nativeModuleInvokeEnd = Number(flow.begin.sliceEndTs);
    }
    if (flow.end.sliceName === NATIVEMODULE_CALLBACK) {
      nativeModuleCallbackEnd = Number(flow.end.sliceEndTs);
    }
  }
  if (
    deprecatedCallbackCallEnd > deprecatedPlatformMethodEnd &&
    deprecatedPlatformMethodEnd > 0
  ) {
    return false;
  }
  if (
    nativeModuleCallbackEnd > nativeModuleInvokeEnd &&
    nativeModuleInvokeEnd > 0
  ) {
    return false;
  }
  return true;
}

export function isNativeModuleCall(name: string) {
  return (
    name === NATIVEMODULE_CALL ||
    name === DEPRECATED_NATIVEMODULE_CALL ||
    name === NATIVEMODULE_NETWORK_REQUEST
  );
}
