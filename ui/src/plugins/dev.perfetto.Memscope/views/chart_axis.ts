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

import m from 'mithril';
import type {SnapshotData} from '../sessions/live_session';
import {chartXWindow, formatRelativeXValue} from '../utils';

// Live charts show a sliding window ending at the latest sample; a loaded
// trace is shown whole, with time relative to the trace start.
export function xAxisFor(
  data: SnapshotData,
  live: boolean,
): {min: number; max: number; format: (v: number) => string} {
  if (live) {
    return {
      ...chartXWindow(data.xMax),
      format: formatRelativeXValue(data.xMax),
    };
  }
  return {
    min: data.xMin,
    max: data.xMax,
    format: (v: number) => `${v.toFixed(0)}s`,
  };
}

export function noData(live: boolean): m.Children {
  return m(
    '.pf-memscope-placeholder',
    live ? 'Waiting for data…' : 'Not recorded in this trace.',
  );
}
