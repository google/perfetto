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

import {findMostRecentScreenshot} from './screenshots_track';

describe('findMostRecentScreenshot', () => {
  const screenshots = [
    {id: 1, ts: 100n},
    {id: 2, ts: 200n},
    {id: 3, ts: 300n},
  ];

  it('finds exact match', () => {
    expect(findMostRecentScreenshot(screenshots, 200n)).toEqual({id: 2, ts: 200n});
  });

  it('finds most recent before timestamp', () => {
    expect(findMostRecentScreenshot(screenshots, 250n)).toEqual({id: 2, ts: 200n});
  });

  it('finds first if timestamp is exactly at first', () => {
    expect(findMostRecentScreenshot(screenshots, 100n)).toEqual({id: 1, ts: 100n});
  });

  it('returns undefined if timestamp is before first', () => {
    expect(findMostRecentScreenshot(screenshots, 50n)).toBeUndefined();
  });

  it('finds last if timestamp is after last', () => {
    expect(findMostRecentScreenshot(screenshots, 400n)).toEqual({id: 3, ts: 300n});
  });

  it('returns undefined if list is empty', () => {
    expect(findMostRecentScreenshot([], 100n)).toBeUndefined();
  });
});
