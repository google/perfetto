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

import {createStore} from '../base/store';
import {createEmptyLynxState} from './empty_state';
import {
  FrameSlice,
  IssueSummary,
  LynxState,
  SliceThreadState,
  VitalTimestampLine,
} from './types';

class LynxPerfGlobals {
  private _store = createStore<LynxState>(createEmptyLynxState());

  appendPerformanceIssue(issues: IssueSummary[]) {
    this._store.edit((draft) => {
      issues.forEach((item) => {
        draft.issues.push(item);
      });
    });
  }

  get state(): LynxState {
    return this._store.state;
  }

  resetIssueStatus() {
    this._store.edit((draft) => {
      Object.assign(draft, createEmptyLynxState());
    });
  }

  updateVitalTimestampLine(timestamp: VitalTimestampLine[]) {
    this._store.edit((draft) => {
      draft.vitalTimestampLine = timestamp;
    });
  }

  updateSelectedTimestamp(timestamp: number) {
    this._store.edit((draft) => {
      draft.selectedTimestamp = timestamp;
    });
  }

  updateSliceThreadMap(sliceThreadMap: Map<string, SliceThreadState>) {
    this._store.edit((draft) => {
      draft.trackUriToThreadMap = sliceThreadMap;
    });
  }

  updateFrameDurationMap(frameDurationMap: Map<number, FrameSlice>) {
    this._store.edit((draft) => {
      draft.frameDurationMap = frameDurationMap;
    });
  }

  addTraceIdToJSBName(traceId: number, name: string) {
    this._store.edit((draft) => {
      draft.traceIdToJSBName.set(traceId, name);
    });
  }
}

export const lynxPerfGlobals = new LynxPerfGlobals();
