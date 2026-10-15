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

import {Time} from '../../base/time';
import {formatWallTimestamp, type WallClock} from './wall_clock';

// 2026-10-14 22:24:04.347 UTC, i.e. 15:24:04.347 PDT.
const START_NS = 1_792_016_644_347_000_000n;

const PDT: WallClock = {
  startEpochNs: START_NS,
  tzOffsetMin: -420,
  tzAbbr: 'PDT',
};

test('formats the wall time in the server time zone', () => {
  expect(formatWallTimestamp(Time.ZERO, PDT)).toBe(
    '2026-10-14 15:24:04.347 PDT (0.000 s)',
  );
  // 7 min 43.4 s later.
  expect(formatWallTimestamp(Time.fromRaw(463_400_000_000n), PDT)).toBe(
    '2026-10-14 15:31:47.747 PDT (463.400 s)',
  );
});

test('crosses midnight with the offset', () => {
  const tokyo = {...PDT, tzOffsetMin: 540, tzAbbr: 'JST'};
  expect(formatWallTimestamp(Time.ZERO, tokyo)).toBe(
    '2026-10-15 07:24:04.347 JST (0.000 s)',
  );
});

test('shows the UTC offset without a zone abbreviation', () => {
  const unnamed = {...PDT, tzAbbr: ''};
  expect(formatWallTimestamp(Time.ZERO, unnamed)).toBe(
    '2026-10-14 15:24:04.347 UTC-07:00 (0.000 s)',
  );
});

test('formats the time since ts 0 without a clock', () => {
  // 1 h 2 min 3.4567 s.
  const ts = Time.fromRaw(3_723_456_700_000n);
  expect(formatWallTimestamp(ts)).toBe('01:02:03.456 (3723.457 s)');
  expect(formatWallTimestamp(ts, {...PDT, startEpochNs: 0n})).toBe(
    '01:02:03.456 (3723.457 s)',
  );
});
