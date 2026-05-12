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

import {isoToEpochMs} from './query_history_storage';

describe('isoToEpochMs', () => {
  test('parses a valid ISO-8601 timestamp', () => {
    const ms = isoToEpochMs('2026-01-02T03:04:05.000Z');
    expect(ms).toBe(Date.UTC(2026, 0, 2, 3, 4, 5));
  });

  test('returns undefined for undefined input', () => {
    expect(isoToEpochMs(undefined)).toBeUndefined();
  });

  test('returns undefined for an unparseable string', () => {
    expect(isoToEpochMs('not-a-date')).toBeUndefined();
  });

  test('returns undefined for a digit-only string (regression)', () => {
    // The earlier bug: query_history.ts stringified an epoch number
    // and query_page.ts then did `new Date(stringOfDigits).getTime()`,
    // yielding NaN. isoToEpochMs must NOT silently parse digit strings.
    expect(isoToEpochMs('1730000000000')).toBeUndefined();
  });

  test('returns undefined for an empty string', () => {
    expect(isoToEpochMs('')).toBeUndefined();
  });
});
