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

import {
  TAsyncTraceEvent,
  TFlowEndTraceEvent,
  TFlowStartTraceEvent,
  TFlowStepTraceEvent,
  TFlowTraceEvent,
  TTraceEvent,
} from '../types';

export enum ETraceEventPhase {
  // Standard
  BEGIN = 'B',
  END = 'E',
  COMPLETE = 'X',
  INSTANCE = 'I',

  // Mark
  MARK = 'R',

  // Async
  ASYNC_NESTABLE_BEGIN = 'b',
  ASYNC_NESTABLE_END = 'e',
  ASYNC_NESTABLE_INSTANCE = 'n',
  FLOW_START = 's',
  FLOW_END = 'f',
  FLOW_STEP = 't',
}

export function isBeginTraceEvent(te: TTraceEvent) {
  return te?.ph === ETraceEventPhase.BEGIN;
}

export function isEndTraceEvent(te: TTraceEvent) {
  return te?.ph === ETraceEventPhase.END;
}

export function isCompleteTraceEvent(te: TTraceEvent) {
  return te?.ph === ETraceEventPhase.COMPLETE;
}

export function isInstanceTraceEvent(te: TTraceEvent) {
  return te?.ph === ETraceEventPhase.INSTANCE;
}

export function isMarkTraceEvent(te: TTraceEvent) {
  return te?.ph === ETraceEventPhase.MARK;
}

export function isAsyncBeginTraceEvent(te: TTraceEvent) {
  return te?.ph === ETraceEventPhase.ASYNC_NESTABLE_BEGIN;
}

export function isAsyncEndTraceEvent(te: TTraceEvent) {
  return te?.ph === ETraceEventPhase.ASYNC_NESTABLE_END;
}

export function isAsyncTraceEvent(te: TTraceEvent): te is TAsyncTraceEvent {
  return (
    te?.ph === ETraceEventPhase.ASYNC_NESTABLE_BEGIN ||
    te?.ph === ETraceEventPhase.ASYNC_NESTABLE_END ||
    te?.ph === ETraceEventPhase.ASYNC_NESTABLE_INSTANCE
  );
}

export function isFlowStartTraceEvent(
  te: TTraceEvent,
): te is TFlowStartTraceEvent {
  return te?.ph === ETraceEventPhase.FLOW_START;
}

export function isFlowStepTraceEvent(
  te: TTraceEvent,
): te is TFlowStepTraceEvent {
  return te?.ph === ETraceEventPhase.FLOW_STEP;
}

export function isFlowEndTraceEvent(te: TTraceEvent): te is TFlowEndTraceEvent {
  return te?.ph === ETraceEventPhase.FLOW_END;
}

export function isFlowTraceEvent(te: TTraceEvent): te is TFlowTraceEvent {
  return (
    te?.ph === ETraceEventPhase.FLOW_END ||
    te?.ph === ETraceEventPhase.FLOW_START ||
    te?.ph === ETraceEventPhase.FLOW_STEP
  );
}
