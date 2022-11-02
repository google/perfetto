// Copyright (C) 2022 The Android Open Source Project
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

import {RunningStatistics} from './perf';

test('buffer size is accurate before reaching max capacity', () => {
  const buf = new RunningStatistics(10);

  for (let i = 0; i < 10; i++) {
    buf.addValue(i);
    expect(buf.bufferSize).toEqual(i + 1);
  }
});

test('buffer size is accurate after reaching max capacity', () => {
  const buf = new RunningStatistics(10);

  for (let i = 0; i < 10; i++) {
    buf.addValue(i);
  }

  for (let i = 0; i < 10; i++) {
    buf.addValue(i);
    expect(buf.bufferSize).toEqual(10);
  }
});

test('buffer mean is accurate before reaching max capacity', () => {
  const buf = new RunningStatistics(10);

  buf.addValue(1);
  buf.addValue(2);
  buf.addValue(3);

  expect(buf.bufferMean).toBeCloseTo(2);
});

test('buffer mean is accurate after reaching max capacity', () => {
  const buf = new RunningStatistics(10);

  for (let i = 0; i < 20; i++) {
    buf.addValue(2);
  }

  expect(buf.bufferMean).toBeCloseTo(2);
});
