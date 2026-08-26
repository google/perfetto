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

import {describe, expect, test} from 'vitest';
import {getTrackName, maybeMachineLabel} from './utils';

describe('maybeMachineLabel', () => {
  test('omits labels for single-machine traces and the host', () => {
    expect(maybeMachineLabel(1, 'guest', 1)).toBe('');
    expect(maybeMachineLabel(0, 'host', 2)).toBe('');
  });

  test('prefers a supplied machine name', () => {
    expect(maybeMachineLabel(1, 'Google Harriet EVT1', 2)).toBe(
      ' (Google Harriet EVT1)',
    );
  });

  test('falls back to the stable machine label index', () => {
    expect(maybeMachineLabel(2, null, 3)).toBe(' (machine 2)');
  });
});

describe('getTrackName', () => {
  test('disambiguates otherwise identical global tracks', () => {
    const common = {name: 'Buffers', numMachines: 2};

    expect(
      getTrackName({...common, machineLabelIndex: 0, machineName: 'host'}),
    ).toBe('Buffers');
    expect(
      getTrackName({
        ...common,
        machineLabelIndex: 1,
        machineName: 'guest',
      }),
    ).toBe('Buffers (guest)');
  });
});
