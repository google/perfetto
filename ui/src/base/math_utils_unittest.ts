// Copyright (C) 2023 The Android Open Source Project
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

import {roundDownNearest, roundUpNearest} from './math_utils';

describe('roundUpNearest()', () => {
  it('rounds decimal values up to the right step size', () => {
    expect(roundUpNearest(0.1, 0.5)).toBeCloseTo(0.5);
    expect(roundUpNearest(17.2, 0.5)).toBeCloseTo(17.5);
  });
});

describe('roundDownNearest()', () => {
  it('rounds decimal values down to the right step size', () => {
    expect(roundDownNearest(0.4, 0.5)).toBeCloseTo(0.0);
    expect(roundDownNearest(17.4, 0.5)).toBeCloseTo(17.0);
  });
});
