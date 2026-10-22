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

import {ANDROID_LOG_STATE_SCHEMA} from './index';

describe('ANDROID_LOG_STATE_SCHEMA', () => {
  test('accepts a fully populated state', () => {
    const state = {
      version: 1,
      filter: {
        minimumLevel: 4,
        tags: ['foo', 'bar'],
        isTagRegex: true,
        textEntry: 'error',
        hideNonMatching: false,
        machineExcludeList: [1, 2],
      },
    };
    expect(ANDROID_LOG_STATE_SCHEMA.parse(state)).toEqual(state);
  });

  test('rejects undefined (relies on defaultValue)', () => {
    expect(ANDROID_LOG_STATE_SCHEMA.safeParse(undefined).success).toBe(false);
  });

  test('rejects a missing version', () => {
    const result = ANDROID_LOG_STATE_SCHEMA.safeParse({filter: {}});
    expect(result.success).toBe(false);
  });

  test('rejects a partial filter', () => {
    const result = ANDROID_LOG_STATE_SCHEMA.safeParse({
      version: 1,
      filter: {textEntry: 'x'},
    });
    expect(result.success).toBe(false);
  });

  test('rejects wrongly typed fields', () => {
    const result = ANDROID_LOG_STATE_SCHEMA.safeParse({
      version: 1,
      filter: {
        minimumLevel: 'high',
        tags: [],
        textEntry: '',
        hideNonMatching: true,
        machineExcludeList: [],
      },
    });
    expect(result.success).toBe(false);
  });
});
