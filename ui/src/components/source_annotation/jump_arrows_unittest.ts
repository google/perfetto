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

import {
  jumpGutterWidthPx,
  layoutJumpArcs,
  renderJumpGutterRow,
} from './jump_arrows';

function insn(relPc: number, targetRelPc?: number) {
  return {
    relPc: BigInt(relPc),
    targetRelPc: targetRelPc === undefined ? undefined : BigInt(targetRelPc),
  };
}

describe('layoutJumpArcs', () => {
  it('ignores instructions without an in-listing target', () => {
    const layout = layoutJumpArcs([insn(0), insn(4, 0x1000), insn(8, 8)]);
    expect(layout.arcs).toEqual([]);
    expect(layout.laneCount).toBe(0);
    expect(jumpGutterWidthPx(layout)).toBe(0);
  });

  it('packs overlapping arcs into lanes, tightest loop innermost', () => {
    // 0: jump to 12 (outer, forward)
    // 4: ...
    // 8: jump to 4 (inner loop, backward)
    // 12: ...
    // 16: jump to 20 (does not overlap the others)
    // 20: ...
    const layout = layoutJumpArcs([
      insn(0, 12),
      insn(4),
      insn(8, 4),
      insn(12),
      insn(16, 20),
      insn(20),
    ]);
    expect(layout.laneCount).toBe(2);
    const byFrom = new Map(layout.arcs.map((a) => [a.fromIndex, a]));
    expect(byFrom.get(2)).toEqual({
      fromIndex: 2,
      toIndex: 1,
      lane: 0,
      backEdge: true,
    });
    expect(byFrom.get(4)).toEqual({
      fromIndex: 4,
      toIndex: 5,
      lane: 0,
      backEdge: false,
    });
    expect(byFrom.get(0)).toEqual({
      fromIndex: 0,
      toIndex: 3,
      lane: 1,
      backEdge: false,
    });
  });
});

describe('renderJumpGutterRow', () => {
  it('draws nothing for rows outside every arc', () => {
    const layout = layoutJumpArcs([insn(0, 4), insn(4), insn(8)]);
    expect(renderJumpGutterRow(2, layout, 24)).toBeUndefined;
  });

  it('draws the connector on the branch and an arrowhead on the target', () => {
    const layout = layoutJumpArcs([insn(0, 8), insn(4), insn(8)]);
    const count = (row: number) => {
      const svg = renderJumpGutterRow(row, layout, 24);
      const paths = (svg as {children: unknown[]}).children;
      return paths.length;
    };
    // Branch row: vertical half plus connector, in one path.
    expect(count(0)).toBe(1);
    // Middle row: vertical only.
    expect(count(1)).toBe(1);
    // Target row: connector path plus the arrowhead.
    expect(count(2)).toBe(2);
  });
});
