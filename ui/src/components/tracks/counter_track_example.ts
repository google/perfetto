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

/**
 * Example usage of CounterTrack - this file demonstrates how to use the
 * CounterTrack implementation and can be used for testing.
 */

import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM} from '../../trace_processor/query_result';
import {CounterTrack, CounterTrackAttrs} from './counter_track';
import {Trace} from '../../public/trace';

// Example: Creating a counter track for CPU usage
export function createCpuUsageCounterTrack(trace: Trace, uri: string) {
  const dataset = new SourceDataset({
    src: `
      SELECT ts, value, cpu
      FROM counter
      WHERE name = 'CPU Usage'
      ORDER BY ts
    `,
    schema: {
      ts: LONG,
      value: NUM,
      cpu: NUM,
    },
  });

  const attrs: CounterTrackAttrs<typeof dataset.schema> = {
    trace,
    uri,
    dataset,
    defaultOptions: {
      yMode: 'value',
      yDisplay: 'zero',
      yRange: 'all',
      unit: '%',
      yOverrideMaximum: 100,
      yOverrideMinimum: 0,
    },
    shellButtons: () => [],
  };

  return CounterTrack.create(attrs);
}

// Example: Creating a memory usage counter track with materialization
export async function createMemoryUsageCounterTrack(trace: Trace, uri: string) {
  const dataset = new SourceDataset({
    src: `
      SELECT 
        ts,
        value / 1024 / 1024 as value,
        name
      FROM counter 
      WHERE name LIKE '%memory%'
      ORDER BY ts
    `,
    schema: {
      ts: LONG,
      value: NUM,
      name: 'STRING',
    },
  });

  const attrs: CounterTrackAttrs<typeof dataset.schema> = {
    trace,
    uri,
    dataset,
    defaultOptions: {
      yMode: 'value',
      yDisplay: 'zero',
      unit: 'MB',
      chartHeightSize: 4,
    },
    tooltip: (row, formattedValue) =>
      `Memory: ${formattedValue} at ${row.name}`,
  };

  // Use materialized version for complex queries
  return CounterTrack.createMaterialized(attrs);
}
