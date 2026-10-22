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

import {STREAM_RESPONSE, TARGETS_RESPONSE} from './wire';

test('fills in proto defaults', () => {
  const resp = STREAM_RESPONSE.parse({completed_slices: [{}]});

  expect(resp.seq).toBe(0n);
  expect(resp.full).toBe(false);
  expect(resp.more).toBe(false);
  expect(resp.tracks).toEqual([]);
  expect(resp.completed_slices).toEqual([
    {
      key: '',
      track_id: '',
      ts_ns: 0n,
      dur_ns: 0n,
      depth: 0,
      name: '',
      category: '',
      args: [],
    },
  ]);
});

test('parses int64 strings and numbers', () => {
  const resp = STREAM_RESPONSE.parse({
    seq: 7,
    playhead_ns: '-5',
    start_epoch_ns: '1760450000123456789',
  });

  expect(resp.seq).toBe(7n);
  expect(resp.playhead_ns).toBe(-5n);
  expect(resp.start_epoch_ns).toBe(1760450000123456789n);

  const limits = STREAM_RESPONSE.parse({
    seq: '9223372036854775807',
    playhead_ns: '-9223372036854775808',
  });
  expect(limits.seq).toBe(2n ** 63n - 1n);
  expect(limits.playhead_ns).toBe(-(2n ** 63n));
});

test('rejects invalid numbers', () => {
  const invalid = [
    {seq: '1.5'},
    {seq: 1.5},
    {seq: ''},
    {seq: '9223372036854775808'},
    {playhead_ns: '-9223372036854775809'},
    {completed_slices: [{depth: 'x'}]},
    {counter_points: [{value: NaN}]},
  ];
  for (const json of invalid) {
    expect(STREAM_RESPONSE.safeParse(json).success).toBe(false);
  }
});

test('ignores unknown track kinds and fields', () => {
  const resp = STREAM_RESPONSE.parse({
    tracks: [{id: 'a', kind: 'KIND_HEATMAP', listed_first: true}],
    new_field: 1,
  });

  expect(resp.tracks).toEqual([
    {id: 'a', name: '', kind: 'KIND_UNSPECIFIED', unit: ''},
  ]);
});

test('parses targets', () => {
  const resp = TARGETS_RESPONSE.parse({
    targets: [{id: 'job-1/worker', task_count: 8}],
  });

  expect(resp.targets[0]!.task_count).toBe(8);
  expect(resp.selected).toBeUndefined();
  expect(resp.page_title).toBe('');
});
