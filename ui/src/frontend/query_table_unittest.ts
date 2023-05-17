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

import {getSliceId, isSliceish} from './query_table';

describe('getSliceId', () => {
  test('get slice_id if present when no other clues are available', () => {
    expect(getSliceId({})).toBe(undefined);
    expect(getSliceId({id: 123})).toBe(undefined);
    expect(getSliceId({slice_id: 456})).toBe(456);
    expect(getSliceId({id: 123, slice_id: 456})).toBe(456);

    expect(getSliceId({type: 'foo'})).toBe(undefined);
    expect(getSliceId({type: 'foo', id: 123})).toBe(undefined);
    expect(getSliceId({type: 'foo', slice_id: 456})).toBe(456);
    expect(getSliceId({type: 'foo', id: 123, slice_id: 456})).toBe(456);
  });

  test('get id if present when row looks like a slice', () => {
    expect(getSliceId({type: 'slice'})).toBe(undefined);
    expect(getSliceId({type: 'slice', id: 123})).toBe(123);
    expect(getSliceId({type: 'slice', slice_id: 456})).toBe(undefined);
    expect(getSliceId({type: 'slice', id: 123, slice_id: 456})).toBe(123);
  });
});

test('isSliceish', () => {
  expect(isSliceish({})).toBeFalsy();
  expect(isSliceish({ts: 123, dur: 456})).toBeFalsy();
  expect(isSliceish({ts: 123, dur: 456, track_id: 798})).toBeTruthy();
  expect(isSliceish({ts: 123n, dur: 456n})).toBeFalsy();
  expect(isSliceish({ts: 123n, dur: 456n, track_id: 798n})).toBeTruthy();
  expect(isSliceish({ts: 123.4, dur: 456.7, track_id: 798.9})).toBeFalsy();
  expect(isSliceish({ts: '123', dur: '456', track_id: '789'})).toBeFalsy();
});
