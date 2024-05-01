// Copyright (C) 2019 The Android Open Source Project
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

import {colorForCpu, colorForThread} from './colorizer';

const PROCESS_A_THREAD_A = {
  tid: 100,
  pid: 100,
};

const PROCESS_A_THREAD_B = {
  tid: 101,
  pid: 100,
};

const PROCESS_B_THREAD_A = {
  tid: 200,
  pid: 200,
};

const PROCESS_UNK_THREAD_A = {
  tid: 42,
};

const PROCESS_UNK_THREAD_B = {
  tid: 42,
};

test('it gives threads colors by pid if present', () => {
  const colorAA = colorForThread(PROCESS_A_THREAD_A);
  const colorAB = colorForThread(PROCESS_A_THREAD_B);
  const colorBA = colorForThread(PROCESS_B_THREAD_A);
  expect(colorAA).toEqual(colorAB);
  expect(colorAA).not.toEqual(colorBA);
});

test('it gives threads colors by tid if pid missing', () => {
  const colorUnkA = colorForThread(PROCESS_UNK_THREAD_A);
  const colorUnkB = colorForThread(PROCESS_UNK_THREAD_B);
  expect(colorUnkA).toEqual(colorUnkB);
});

test("it doesn't copy colors", () => {
  const a = colorForThread(PROCESS_A_THREAD_A);
  const b = colorForThread(PROCESS_A_THREAD_A);
  expect(a).toBe(b);
});

test('it gives different cpus different hues', () => {
  expect(colorForCpu(0)).not.toEqual(colorForCpu(1));
});
