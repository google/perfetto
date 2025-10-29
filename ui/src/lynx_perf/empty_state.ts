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

import {RIGHT_SIDEBAR_MIN_WIDTH} from './constants';
import {LynxState, RightSidebarTab} from './types';

export function createEmptyLynxState(): LynxState {
  return {
    issues: [],
    vitalTimestampLine: [],
    selectedTimestamp: -1,
    traceIdToJSBName: new Map(),
    traceIdToScrollName: new Map(),
    trackUriToThreadMap: new Map(),
    // From Lynx 3.4, we delete trace events for NativeModule timing.
    nonTimingNativeModuleTraces: false,
    frameDurationMap: new Map(),
    highlightNoInstanceIdTrace: true,
    lynxviewInstances: [],
    selectedLynxviewInstances: [],
    filteredTraceSet: new Set(),
    showRightSidebar: false,
    rightSidebarTab: RightSidebarTab.Unknown,
    rightSidebarWidth: RIGHT_SIDEBAR_MIN_WIDTH,
  };
}
