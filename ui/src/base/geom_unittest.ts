// Copyright (C) 2024 The Android Open Source Project
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

import {intersectRects, expandRect, rebaseRect, rectSize, Rect} from './geom';

describe('intersectRects', () => {
  it('should correctly intersect two overlapping rects', () => {
    const a: Rect = {left: 1, top: 1, right: 4, bottom: 4};
    const b: Rect = {left: 2, top: 2, right: 5, bottom: 5};
    const result = intersectRects(a, b);
    expect(result).toEqual({left: 2, top: 2, right: 4, bottom: 4});
  });
  // Note: Non-overlapping rects are not supported and thus not tested
});

describe('expandRect', () => {
  it('should correctly expand a rect by a given amount', () => {
    const rect: Rect = {left: 1, top: 1, right: 3, bottom: 3};
    const amount = 1;
    const result = expandRect(rect, amount);
    expect(result).toEqual({left: 0, top: 0, right: 4, bottom: 4});
  });
});

describe('rebaseRect', () => {
  it('should correctly rebase a rect', () => {
    const rect: Rect = {left: 2, top: 2, right: 5, bottom: 5};
    const x = 1;
    const y = 1;
    const result = rebaseRect(rect, x, y);
    expect(result).toEqual({left: 1, top: 1, right: 4, bottom: 4});
  });
});

describe('rectSize', () => {
  it('should correctly calculate the size of a rect', () => {
    const rect: Rect = {left: 1, top: 1, right: 4, bottom: 3};
    const result = rectSize(rect);
    expect(result).toEqual({width: 3, height: 2});
  });
});
