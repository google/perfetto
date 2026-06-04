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

import {getDiffColorCss} from './flamegraph';

// getDiffColorCss returns the HSLColor cssString, i.e. "rgb(R G B)".
function rgb(score: number): [number, number, number] {
  const css = getDiffColorCss(score);
  const m = css.match(/rgb\((\d+)\s+(\d+)\s+(\d+)/);
  if (m === null) throw new Error(`unexpected colour string: ${css}`);
  return [Number(m[1]), Number(m[2]), Number(m[3])];
}

describe('flamegraph pprof-style diff colour', () => {
  test('zero score is neutral grey', () => {
    const [r, g, b] = rgb(0);
    expect(r).toBe(g);
    expect(g).toBe(b);
  });

  test('positive score (grew) is red-dominant', () => {
    const [r, g, b] = rgb(1);
    expect(r).toBeGreaterThan(g);
    expect(r).toBeGreaterThan(b);
  });

  test('negative score (shrank) is green-dominant', () => {
    const [r, g, b] = rgb(-1);
    expect(g).toBeGreaterThan(r);
    expect(g).toBeGreaterThan(b);
  });

  test('intensity is monotonic in |score|', () => {
    // More growth ⇒ the green channel drops further (more vivid red).
    expect(rgb(0.9)[1]).toBeLessThan(rgb(0.3)[1]);
    // More shrink ⇒ the red channel drops further (more vivid green).
    expect(rgb(-0.9)[0]).toBeLessThan(rgb(-0.3)[0]);
  });

  test('clamps out-of-range and non-finite scores', () => {
    expect(getDiffColorCss(5)).toBe(getDiffColorCss(1));
    expect(getDiffColorCss(-5)).toBe(getDiffColorCss(-1));
    const [r, g, b] = rgb(NaN);
    expect(r).toBe(g);
    expect(g).toBe(b);
  });
});
