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

import {computeStatus} from './status';

test('computeStatus prefers error, then paused, then buffering', () => {
  const live = {paused: false, lastDataMs: 0, nowMs: 5000};

  expect(computeStatus(live)).toBe('live');
  expect(computeStatus({...live, error: 'x', paused: true})).toBe('error');
  expect(computeStatus({...live, paused: true, lastDataMs: undefined})).toBe(
    'paused',
  );
  expect(computeStatus({...live, lastDataMs: undefined})).toBe('buffering');
  expect(computeStatus({...live, nowMs: 5001})).toBe('buffering');
});

test('computeStatus buffers while the server has no data yet', () => {
  const waiting = {paused: false, waiting: true, lastDataMs: 0, nowMs: 0};

  expect(computeStatus(waiting)).toBe('buffering');
  expect(computeStatus({...waiting, paused: true})).toBe('paused');
  expect(computeStatus({...waiting, error: 'x'})).toBe('error');
});
