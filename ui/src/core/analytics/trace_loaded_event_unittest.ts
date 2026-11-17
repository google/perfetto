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

import {createFakeTraceImpl} from '../fake_trace_impl';
import type {TraceInfoImpl} from '../trace_info_impl';
import {buildTraceLoadedEvent} from './trace_loaded_event';

const MB = 1e6;

// A loaded trace's details, with the fields the event reads set as given.
function traceInfo(
  traceTypes: string[],
  fileSizeBytes?: number,
): TraceInfoImpl {
  return {...createFakeTraceImpl().traceInfo, traceTypes, fileSizeBytes};
}

describe('buildTraceLoadedEvent', () => {
  test('should send the trace load ID and the load time', () => {
    const info = traceInfo(['proto'], 1 * MB);
    const event = buildTraceLoadedEvent(info, 8420.4);
    expect(event.kind).toBe('trace_loaded');
    expect(event.traceLoadId).toBe(info.traceLoadId);
    expect(event.loadMs).toBe(8420);
  });

  test('should sort and join the trace types', () => {
    expect(buildTraceLoadedEvent(traceInfo(['proto']), 0).traceFormat).toBe(
      'proto',
    );
    expect(
      buildTraceLoadedEvent(traceInfo(['proto', 'json']), 0).traceFormat,
    ).toBe('json+proto');
  });

  test('should put the trace size in a bucket', () => {
    const bucket = (bytes?: number) =>
      buildTraceLoadedEvent(traceInfo(['proto'], bytes), 0).traceSizeBucket;
    expect(bucket(10 * MB - 1)).toBe('<10MB');
    expect(bucket(10 * MB)).toBe('10-100MB');
    expect(bucket(100 * MB - 1)).toBe('10-100MB');
    expect(bucket(100 * MB)).toBe('100MB-1GB');
    expect(bucket(1000 * MB - 1)).toBe('100MB-1GB');
    expect(bucket(1000 * MB)).toBe('>1GB');
  });

  test('should report an unknown size when the size is not known', () => {
    expect(buildTraceLoadedEvent(traceInfo(['proto']), 0).traceSizeBucket).toBe(
      'unknown',
    );
  });
});
