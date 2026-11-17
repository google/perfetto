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

import type {TraceLoadedEvent, TraceSizeBucket} from './analytics_impl';
import type {TraceInfoImpl} from '../trace_info_impl';

const MB = 1e6;

// Builds the trace_loaded analytics event for a trace that took |loadMs| to
// load, from the start of loading until every plugin's onTraceLoad finished.
export function buildTraceLoadedEvent(
  traceInfo: TraceInfoImpl,
  loadMs: number,
): TraceLoadedEvent {
  return {
    kind: 'trace_loaded',
    traceLoadId: traceInfo.traceLoadId,
    // A trace can have several types, e.g. when several traces are opened
    // together. Sorting keeps the same mix of types reported the same way.
    traceFormat: [...traceInfo.traceTypes].sort().join('+'),
    traceSizeBucket: toSizeBucket(traceInfo.fileSizeBytes),
    loadMs: Math.round(loadMs),
  };
}

function toSizeBucket(bytes: number | undefined): TraceSizeBucket {
  // The size is only known in advance for traces opened from a file or a
  // buffer, not from a URL, a stream or a trace processor connection.
  if (bytes === undefined) return 'unknown';
  if (bytes < 10 * MB) return '<10MB';
  if (bytes < 100 * MB) return '10-100MB';
  if (bytes < 1000 * MB) return '100MB-1GB';
  return '>1GB';
}
