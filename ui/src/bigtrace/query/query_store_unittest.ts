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

import {describe, expect, test} from 'vitest';
import {queryStore} from './query_store';

describe('queryStore', () => {
  test('ignores empty schema arrays from history merges', () => {
    const uuid = 'schema-preserve-uuid';
    queryStore.getOrCreate(uuid, {
      status: 'SUCCESS',
      processedRows: 50,
      schema: [{name: 'dur', type: 'INT64'}],
    });
    // History list responses carry an empty schema.
    queryStore.getOrCreate(uuid, {
      status: 'SUCCESS',
      processedRows: 50,
      schema: [],
    });
    queryStore.update(uuid, {schema: []});

    expect(queryStore.getOrCreate(uuid).schema).toEqual([
      {name: 'dur', type: 'INT64'},
    ]);
  });

  test('advances through PREVIEW and FULL_TABLE without regressing on stale snapshots', () => {
    const uuid = 'stage-monotonic-uuid';
    queryStore.getOrCreate(uuid, {
      status: 'IN_PROGRESS',
      processedRows: 0,
      processedTraces: 0,
      totalTraces: 10000,
    });
    // Entering PREVIEW with 0 rows advances from undefined phase.
    queryStore.getOrCreate(uuid, {
      status: 'IN_PROGRESS',
      processedRows: 1000,
      processedTraces: 850,
      totalTraces: 10000,
      phase: 'PREVIEW',
    });
    expect(queryStore.getOrCreate(uuid).phase).toBe('PREVIEW');

    // FULL_TABLE starts: processedTraces resets while phase advances.
    queryStore.getOrCreate(uuid, {
      status: 'IN_PROGRESS',
      processedRows: 1000,
      processedTraces: 3,
      totalTraces: 10000,
      phase: 'FULL_TABLE',
    });
    expect(queryStore.getOrCreate(uuid).phase).toBe('FULL_TABLE');
    expect(queryStore.getOrCreate(uuid).processedTraces).toBe(3);

    // A stale PREVIEW or phase-less list entry must not rewind phase or traces.
    queryStore.getOrCreate(uuid, {
      status: 'IN_PROGRESS',
      processedRows: 1000,
      processedTraces: 850,
      totalTraces: 10000,
      phase: 'PREVIEW',
    });
    expect(queryStore.getOrCreate(uuid).phase).toBe('FULL_TABLE');
    expect(queryStore.getOrCreate(uuid).processedTraces).toBe(3);

    // Terminal status clears phase.
    queryStore.getOrCreate(uuid, {
      status: 'SUCCESS',
      processedRows: 5000,
      processedTraces: 10000,
      totalTraces: 10000,
    });
    expect(queryStore.getOrCreate(uuid).phase).toBeUndefined();
  });
});
