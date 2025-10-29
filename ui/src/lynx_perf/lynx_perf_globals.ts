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
  LynxViewInstance,
  RightSidebarTab,
  SliceThreadState,
  VitalTimestampLine,
} from './types';

class LynxPerfGlobals {
  private _store = createStore<LynxState>(createEmptyLynxState());

  reset() {
    this._store.edit((draft) => {
      Object.assign(draft, createEmptyLynxState());
    });
    this.closeRightSidebar();
  }

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

  setNonTimingNativeModuleTraces(nonTimingNativeModuleTraces: boolean) {
    this._store.edit((draft) => {
      draft.nonTimingNativeModuleTraces = nonTimingNativeModuleTraces;
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
      frameDurationMap.forEach((value, key) => {
        draft.frameDurationMap.set(key, value);
      });
    });
  }

  addTraceIdToJSBName(traceId: number, name: string) {
    this._store.edit((draft) => {
      draft.traceIdToJSBName.set(traceId, name);
    });
  }

  setTraceIdToScrollName(traceId: number, name: string) {
    this._store.edit((draft) => {
      draft.traceIdToScrollName.set(traceId, name);
    });
  }

  updateFilteredTraceSet(set: Set<number>) {
    this._store.edit((draft) => {
      draft.filteredTraceSet = set;
    });
  }

  updateLynxViewInstances(instances: LynxViewInstance[]) {
    this._store.edit((draft) => {
      draft.lynxviewInstances = instances;
    });
  }

  updateSelectedLynxViewInstances(instances: LynxViewInstance[]) {
    this._store.edit((draft) => {
      draft.selectedLynxviewInstances = instances;
    });
  }

  shouldShowSlice(sliceId: number) {
    return (
      this._store.state.filteredTraceSet.size <= 0 ||
      !this._store.state.filteredTraceSet.has(sliceId)
    );
  }

  setHighlightNoInstanceIdTrace(showOtherTrace: boolean) {
    this._store.edit((draft) => {
      draft.highlightNoInstanceIdTrace = showOtherTrace;
    });
  }

  closeRightSidebar() {
    this._store.edit((draft) => {
      draft.showRightSidebar = false;
      draft.rightSidebarTab = RightSidebarTab.Unknown;
    });

    document.documentElement.style.setProperty('--right-sidebar-width', '0px');
  }

  changeRightSidebarTab(tab: RightSidebarTab) {
    this._store.edit((draft) => {
      draft.rightSidebarTab = tab;
      draft.showRightSidebar = true;
    });
    document.documentElement.style.setProperty(
      '--right-sidebar-width',
      this._store.state.rightSidebarWidth + 'px',
    );
  }

  changeRightSidebarWidth(width: number) {
    this._store.edit((draft) => {
      draft.rightSidebarWidth = width;
    });
  }
}

export const lynxPerfGlobals = new LynxPerfGlobals();
