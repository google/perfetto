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
  it('returns needsUpdate=true on first call with no data', () => {
    const bounds = new BufferedBounds();
    const result = bounds.update(mkSpan(100n, 200n), 1n, false);

    expect(result.needsUpdate).toBe(true);
  });

  it('returns needsUpdate=true on first call even with hasData=true', () => {
    // hasData=true but bounds haven't been set yet, so visible window
    // exceeds loaded bounds (which are 0-0)
    const bounds = new BufferedBounds();
    const result = bounds.update(mkSpan(100n, 200n), 1n, true);

    expect(result.needsUpdate).toBe(true);
  });

  it('pads bounds by visible duration on each side (3x total)', () => {
    const bounds = new BufferedBounds();
    // Visible span: 100-200, duration=100
    // Padded: 0-300 (100-100, 200+100)
    const result = bounds.update(mkSpan(100n, 200n), 1n, false);

    expect(result.start).toBe(t(0n));
    expect(result.end).toBe(t(300n));
  });

  it('quantizes bounds to resolution', () => {
    const bounds = new BufferedBounds();
    // Visible span: 105-195, duration=90
    // Padded: 15-285
    // Quantized with resolution=10: floor(15/10)*10=10, ceil(285/10)*10=290
    const result = bounds.update(mkSpan(105n, 195n), 10n, false);

    expect(result.start).toBe(t(10n));
    expect(result.end).toBe(t(290n));
  });

  it('returns needsUpdate=false when visible window is within loaded bounds', () => {
    const bounds = new BufferedBounds();

    // First update sets bounds
    bounds.update(mkSpan(100n, 200n), 1n, false);

    // Second update within bounds (original was 100-200, padded to 0-300)
    const result = bounds.update(mkSpan(50n, 250n), 1n, true);

    expect(result.needsUpdate).toBe(false);
    // Bounds should remain unchanged
    expect(result.start).toBe(t(0n));
    expect(result.end).toBe(t(300n));
  });

  it('returns needsUpdate=true when visible window exceeds loaded start', () => {
    const bounds = new BufferedBounds();

    // First update: visible 100-200 -> padded 0-300
    bounds.update(mkSpan(100n, 200n), 1n, false);

    // Pan left beyond loaded bounds
    const result = bounds.update(mkSpan(-50n, 50n), 1n, true);

    expect(result.needsUpdate).toBe(true);
    // Should recalculate: visible -50 to 50, duration=100
    // Padded: -150 to 150
    expect(result.start).toBe(t(-150n));
    expect(result.end).toBe(t(150n));
  });

  it('returns needsUpdate=true when visible window exceeds loaded end', () => {
    const bounds = new BufferedBounds();

    // First update: visible 100-200 -> padded 0-300
    bounds.update(mkSpan(100n, 200n), 1n, false);

    // Pan right beyond loaded bounds
    const result = bounds.update(mkSpan(250n, 350n), 1n, true);

    expect(result.needsUpdate).toBe(true);
    // Should recalculate: visible 250-350, duration=100
    // Padded: 150-450
    expect(result.start).toBe(t(150n));
    expect(result.end).toBe(t(450n));
  });

  it('returns needsUpdate=true when resolution changes', () => {
    const bounds = new BufferedBounds();

    // First update with resolution 1
    bounds.update(mkSpan(100n, 200n), 1n, false);

    // Same visible window but different resolution
    const result = bounds.update(mkSpan(100n, 200n), 10n, true);

    expect(result.needsUpdate).toBe(true);
    expect(result.resolution).toBe(10n);
    // Bounds recalculated and quantized to new resolution
    expect(result.start).toBe(t(0n));
    expect(result.end).toBe(t(300n));
  });

  it('reset() forces refetch on next update', () => {
    const bounds = new BufferedBounds();

    // First update
    bounds.update(mkSpan(100n, 200n), 1n, false);

    // Verify within bounds
    let result = bounds.update(mkSpan(100n, 200n), 1n, true);
    expect(result.needsUpdate).toBe(false);

    // Reset
    bounds.reset();

    // Same call should now need update
    result = bounds.update(mkSpan(100n, 200n), 1n, true);
    expect(result.needsUpdate).toBe(true);
  });

  it('bounds getter returns current values without updating', () => {
    const bounds = new BufferedBounds();

    // Initial state
    expect(bounds.bounds.start).toBe(t(0n));
    expect(bounds.bounds.end).toBe(t(0n));
    expect(bounds.bounds.resolution).toBe(0n);

    // Update
    bounds.update(mkSpan(100n, 200n), 5n, false);

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
    bounds.update(mkSpan(100n, 110n), 1n, false);

    // Zoom out significantly
    const result = bounds.update(mkSpan(0n, 1000n), 10n, true);

    expect(result.needsUpdate).toBe(true);
    // Visible: 0-1000, duration=1000
    // Padded: -1000 to 2000
    // Quantized to 10: -1000 to 2000
    expect(result.start).toBe(t(-1000n));
    expect(result.end).toBe(t(2000n));
  });

  it('handles hasData=false forcing update even within bounds', () => {
    const bounds = new BufferedBounds();

    // First update
    bounds.update(mkSpan(100n, 200n), 1n, false);

    // Same call with hasData=false should force update
    const result = bounds.update(mkSpan(100n, 200n), 1n, false);
    expect(result.needsUpdate).toBe(true);
  });
});
