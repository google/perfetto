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
import {
  shownTreeExplorerProfileIndex,
  stepTreeExplorerProfile,
} from './tree_explorer_merge';

const PROFILES = [
  {key: 'a', label: 'a.pprof'},
  {key: 'b', label: 'b.pprof'},
  {key: 'c', label: 'c.pprof'},
];

describe('merge selection', () => {
  test('shows the selected profile, else the first', () => {
    const index = (profileKey?: string) =>
      shownTreeExplorerProfileIndex(PROFILES, {merged: false, profileKey});
    expect(index('b')).toBe(1);
    expect(index('c')).toBe(2);
    expect(index(undefined)).toBe(0);
    // E.g. persisted for a profile the trace no longer has.
    expect(index('gone')).toBe(0);
  });

  test('steps through the profiles in order, stopping at the ends', () => {
    const step = (profileKey: string | undefined, by: number) =>
      stepTreeExplorerProfile(PROFILES, {merged: false, profileKey}, by);
    expect(step('a', 1)).toEqual({merged: false, profileKey: 'b'});
    expect(step('b', 1)).toEqual({merged: false, profileKey: 'c'});
    expect(step('c', -1)).toEqual({merged: false, profileKey: 'b'});
    expect(step('c', 1)).toBeUndefined();
    expect(step('a', -1)).toBeUndefined();
    // No or an unknown key means the first profile.
    expect(step(undefined, 1)).toEqual({merged: false, profileKey: 'b'});
    expect(step('gone', -1)).toBeUndefined();
  });

  test('does not step through the profiles while merged', () => {
    expect(
      stepTreeExplorerProfile(PROFILES, {merged: true, profileKey: 'a'}, 1),
    ).toBeUndefined();
  });
});
