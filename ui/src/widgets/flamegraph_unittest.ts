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

import {diffColorStep} from './flamegraph';

test('diffColorStep is positive when a node grew, negative when it shrank', () => {
  expect(diffColorStep(20, 10)).toBe(5);
  expect(diffColorStep(10, 20)).toBe(-5);
  expect(diffColorStep(10, 0)).toBe(10);
  expect(diffColorStep(0, 10)).toBe(-10);
});

test('diffColorStep ignores small changes', () => {
  expect(diffColorStep(102, 100)).toBe(0);
  expect(diffColorStep(100, 102)).toBe(0);
  expect(diffColorStep(0, 0)).toBe(0);
});
