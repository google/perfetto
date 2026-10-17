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

import {expandProcessName} from './metricUtils';

const expandProcessNameTest: {input: string; expectedOutput: string}[] = [
  {input: 'systemui', expectedOutput: 'com.android.systemui'},
  {input: 'com.android.systemui', expectedOutput: 'com.android.systemui'},
  {
    input: 'systemui_stl',
    expectedOutput: 'com.android.compose.animation.scene.demo.app',
  },
  {input: 'uibench', expectedOutput: 'com.android.test.uibench'},
  {input: 'launcher', expectedOutput: 'com.google.android.apps.nexuslauncher'},
  {input: 'surfaceflinger', expectedOutput: '/system/bin/surfaceflinger'},
  {input: 'system_server', expectedOutput: 'system_server'},
];

describe('expandProcessName', () => {
  it('expands abbreviated process names', () => {
    for (const testCase of expandProcessNameTest) {
      expect(expandProcessName(testCase.input)).toEqual(
        testCase.expectedOutput,
      );
    }
  });
});
