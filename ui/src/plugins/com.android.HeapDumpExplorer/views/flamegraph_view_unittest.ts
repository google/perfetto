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

import {Time} from '../../../base/time';
import {buildHeapGraphMetrics} from './flamegraph_view';

const DUMP_A = {upid: 1, ts: Time.fromRaw(100n)};
const DUMP_B = {upid: 2, ts: Time.fromRaw(200n)};

function groupingColumns(
  dump: typeof DUMP_A,
  baselineDump?: typeof DUMP_A,
): string[] {
  return buildHeapGraphMetrics(dump, baselineDump, () => {}).flatMap((metric) =>
    (metric.unaggregatableProperties ?? []).map((p) => p.name),
  );
}

test('one dump groups nodes by their path hash', () => {
  expect(groupingColumns(DUMP_A)).toContain('path_hash_stable');
});

test('compared dumps are not grouped by the per-dump path hash', () => {
  expect(groupingColumns(DUMP_A, DUMP_B)).not.toContain('path_hash_stable');
});
