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

export interface LynxState {
  issues: IssueSummary[];
  vitalTimestampLine: VitalTimestampLine[];
  selectedTimestamp: number;

  // NativeModule
  traceIdToJSBName: Map<number, string>;
  traceIdToScrollName: Map<number, string>;
  trackUriToThreadMap: Map<string, SliceThreadState>;
  nonTimingNativeModuleTraces: boolean;

  frameDurationMap: Map<number, FrameSlice>;

  highlightNoInstanceIdTrace: boolean;
  lynxviewInstances: LynxViewInstance[];
  selectedLynxviewInstances: LynxViewInstance[];
  filteredTraceSet: Set<number>;

  showRightSidebar: boolean;
}

export interface SliceThreadState {
  utid: number;
  upid: number;
  tid: number;
  trackName: string;
  trackId: number;
  isMainThread: boolean;
  isKernelThread: boolean;
  threadName: string;
}

export interface FrameSlice {
  dur: number;
  id: number;
  trackId: number;
}

export enum IssueRank {
  MINOR,
  MODERATE,
  CRITICAL,
}

export interface IssueSummary extends BaseSlice {
  id: number;
  ts: number;
  issueRank: IssueRank;
  trackUri: string;
}

export interface BaseSlice {
  id: number;
  ts: number;
  dur?: number;
  tooltip?: string;
  highlighted?: boolean;
  argSetId?: number | null;
}

export interface VitalTimestamp extends BaseSlice {
  name: string[];
  trackId: number;
  instanceId?: number;
  pipelineId: string;
  widthPx?: number;
}

export interface VitalTimestampLine {
  name: string[];
  ts: number;
  id: number;
}

export interface LynxViewInstance {
  url: string;
  instanceId: string;
}
