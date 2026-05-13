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

import {Time, TimeSpan} from '../../base/time';
import {BufferedBounds} from './buffered_bounds';

const t = Time.fromRaw;

function mkSpan(start: bigint, end: bigint) {
  return new TimeSpan(t(start), t(end));
}

describe('BufferedBounds', () => {
  it('pads bounds by visible duration on each side (3x total)', () => {
    const bounds = new BufferedBounds();
    // Visible span: 100-200, duration=100
    // Padded: 0-300 (100-100, 200+100)
    const result = bounds.update(mkSpan(100n, 200n), 1n);

    expect(result.start).toBe(t(0n));
    expect(result.end).toBe(t(300n));
  });

  it('quantizes bounds to resolution', () => {
    const bounds = new BufferedBounds();
    // Visible span: 105-195, duration=90
    // Padded: 15-285
    // Quantized with resolution=10: floor(15/10)*10=10, ceil(285/10)*10=290
    const result = bounds.update(mkSpan(105n, 195n), 10n);

    expect(result.start).toBe(t(10n));
    expect(result.end).toBe(t(290n));
  });

  it('does not change bounds when visible window is within loaded bounds', () => {
    const bounds = new BufferedBounds();

    // First update sets bounds: visible 100-200 -> padded 0-300
    bounds.update(mkSpan(100n, 200n), 1n);

    // Second update within bounds
    const result = bounds.update(mkSpan(50n, 250n), 1n);

    // Bounds should remain unchanged
    expect(result.start).toBe(t(0n));
    expect(result.end).toBe(t(300n));
  });

  it('recalculates bounds when visible window exceeds loaded start', () => {
    const bounds = new BufferedBounds();

    // First update: visible 100-200 -> padded 0-300
    bounds.update(mkSpan(100n, 200n), 1n);

    // Pan left beyond loaded bounds
    const result = bounds.update(mkSpan(-50n, 50n), 1n);

    // Should recalculate: visible -50 to 50, duration=100
    // Padded: -150 to 150
    expect(result.start).toBe(t(-150n));
    expect(result.end).toBe(t(150n));
  });

  it('recalculates bounds when visible window exceeds loaded end', () => {
    const bounds = new BufferedBounds();

    // First update: visible 100-200 -> padded 0-300
    bounds.update(mkSpan(100n, 200n), 1n);

    // Pan right beyond loaded bounds
    const result = bounds.update(mkSpan(250n, 350n), 1n);

    // Should recalculate: visible 250-350, duration=100
    // Padded: 150-450
    expect(result.start).toBe(t(150n));
    expect(result.end).toBe(t(450n));
  });

  it('recalculates bounds when resolution changes', () => {
    const bounds = new BufferedBounds();

    // First update with resolution 1
    bounds.update(mkSpan(100n, 200n), 1n);

    // Same visible window but different resolution
    const result = bounds.update(mkSpan(100n, 200n), 10n);

    expect(result.resolution).toBe(10n);
    // Bounds recalculated and quantized to new resolution
    expect(result.start).toBe(t(0n));
    expect(result.end).toBe(t(300n));
  });

  it('reset() forces refetch on next update', () => {
    const bounds = new BufferedBounds();

    // First update: visible 100-200 -> padded 0-300
    bounds.update(mkSpan(100n, 200n), 1n);

    // Verify within bounds - should not change
    let result = bounds.update(mkSpan(100n, 200n), 1n);
    expect(result.start).toBe(t(0n));
    expect(result.end).toBe(t(300n));

    // Reset
    bounds.reset();

    // Same call should recalculate bounds
    result = bounds.update(mkSpan(100n, 200n), 1n);
    // Bounds recalculated fresh
    expect(result.start).toBe(t(0n));
    expect(result.end).toBe(t(300n));
  });

  it('bounds getter returns current values without updating', () => {
    const bounds = new BufferedBounds();

    // Initial state
    expect(bounds.bounds.start).toBe(t(0n));
    expect(bounds.bounds.end).toBe(t(0n));
    expect(bounds.bounds.resolution).toBe(0n);

    // Update
    bounds.update(mkSpan(100n, 200n), 5n);

    // Getter should return updated values
    expect(bounds.bounds.start).toBe(t(0n));
    expect(bounds.bounds.end).toBe(t(300n));
    expect(bounds.bounds.resolution).toBe(5n);

    // Getter should not change values
    const boundsAgain = bounds.bounds;
    expect(boundsAgain.start).toBe(t(0n));
    expect(boundsAgain.end).toBe(t(300n));
  });

  it('handles large zoom out (larger visible duration)', () => {
    const bounds = new BufferedBounds();

    // Start with narrow view
    bounds.update(mkSpan(100n, 110n), 1n);

    // Zoom out significantly
    const result = bounds.update(mkSpan(0n, 1000n), 10n);

    // Visible: 0-1000, duration=1000
    // Padded: -1000 to 2000
    // Quantized to 10: -1000 to 2000
    expect(result.start).toBe(t(-1000n));
    expect(result.end).toBe(t(2000n));
  });
});
