// Copyright (C) 2024 The Android Open Source Project
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

import {time} from '../base/time';

export interface TraceContext {
  traceTitle: string; // File name and size of the current trace.
  traceUrl: string; // URL of the Trace.
  readonly start: time;
  readonly end: time;

  // This is the ts value at the time of the Unix epoch.
  // Normally some large negative value, because the unix epoch is normally in
  // the past compared to ts=0.
  readonly realtimeOffset: time;

  // This is the timestamp that we should use for our offset when in UTC mode.
  // Usually the most recent UTC midnight compared to the trace start time.
  readonly utcOffset: time;

  // Trace TZ is like UTC but keeps into account also the timezone_off_mins
  // recorded into the trace, to show timestamps in the device local time.
  readonly traceTzOffset: time;

  // The list of CPUs in the trace
  readonly cpus: number[];

  // The number of gpus in the trace
  readonly gpuCount: number;
}
