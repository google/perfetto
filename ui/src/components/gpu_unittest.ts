// Copyright (C) 2025 The Android Open Source Project
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

import {Gpu} from './gpu';

test('displayName returns GPU name when set', () => {
  const gpu = new Gpu(1, 0, 0, 'NVIDIA A100');
  expect(gpu.displayName).toBe('NVIDIA A100');
});

test('displayName falls back to GPU index when name is undefined', () => {
  const gpu = new Gpu(1, 0, 0);
  expect(gpu.displayName).toBe('GPU 0');
});

test('displayName falls back to GPU index when name is not provided', () => {
  const gpu = new Gpu(2, 3, 0);
  expect(gpu.displayName).toBe('GPU 3');
});

test('displayName with empty string name uses empty string', () => {
  const gpu = new Gpu(1, 0, 0, '');
  expect(gpu.displayName).toBe('GPU 0');
});
