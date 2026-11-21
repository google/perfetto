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

import type {TraceLoadId} from './analytics/analytics_impl';
import type {TraceSource} from '../core/trace_source';
import type {TraceInfo} from '../public/trace_info';

export interface TraceInfoImpl extends TraceInfo {
  readonly source: TraceSource;

  // A random ID for this load of the trace, created when loading starts.
  // Loading the same trace again gets a new ID. Not to be confused with
  // TraceInfo.uuid, which identifies the trace itself and is the same on every
  // load.
  readonly traceLoadId: TraceLoadId;
}
