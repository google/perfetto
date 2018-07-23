// Copyright (C) 2018 The Android Open Source Project
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

import {GridlineHelper} from './gridline_helper';

test('gridline helper to have sensible step sizes', () => {
  expect(GridlineHelper.getStepSize(10)).toEqual(1);
  expect(GridlineHelper.getStepSize(30)).toEqual(2);
  expect(GridlineHelper.getStepSize(60)).toEqual(5);
  expect(GridlineHelper.getStepSize(100)).toEqual(10);
});

test('gridline helper to scale to very small and very large values', () => {
  expect(GridlineHelper.getStepSize(.01)).toEqual(.001);
  expect(GridlineHelper.getStepSize(10000)).toEqual(1000);
});

test('gridline helper to always return a reasonable number of steps', () => {
  for (let i = 1; i <= 1000; i++) {
    const stepSize = GridlineHelper.getStepSize(i);
    expect(Math.round(i / stepSize)).toBeGreaterThanOrEqual(10);
    expect(Math.round(i / stepSize)).toBeLessThanOrEqual(25);
  }
});
