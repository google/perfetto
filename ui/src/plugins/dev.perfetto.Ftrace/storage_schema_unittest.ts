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

import {FTRACE_STATE_SCHEMA} from './common';

describe('FTRACE_STATE_SCHEMA', () => {
  test('accepts the default state', () => {
    const state = {version: 2, filter: {excludeList: []}};
    expect(FTRACE_STATE_SCHEMA.parse(state)).toEqual(state);
  });

  test('accepts a populated state', () => {
    const state = {
      version: 2,
      filter: {
        excludeList: ['sched_switch', 'sched_wakeup'],
        visibleCpus: [0, 2],
      },
    };
    expect(FTRACE_STATE_SCHEMA.parse(state)).toEqual(state);
  });

  test('rejects other versions', () => {
    const result = FTRACE_STATE_SCHEMA.safeParse({
      version: 1,
      filter: {excludeList: []},
    });
    expect(result.success).toBe(false);
  });
});
