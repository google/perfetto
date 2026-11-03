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

// Arrows from each branch in a disassembly listing to its target, drawn in a
// gutter to the left of the instructions like `objdump --visualize-jumps`.
// Arcs are packed into lanes so that overlapping ones never collide, with
// tighter loops in the lanes closest to the code. Each row draws only its
// own slice of the gutter, so the listing can be virtualized.

import m from 'mithril';

export interface JumpArc {
  // Row indices of the branch and its target.
  readonly fromIndex: number;
  readonly toIndex: number;
  // Lane 0 is closest to the code; higher lanes are further left.
  readonly lane: number;
  // The target is at or before the branch: a loop.
  readonly backEdge: boolean;
}

export interface JumpArcLayout {
  readonly arcs: ReadonlyArray<JumpArc>;
  readonly laneCount: number;
}

const EMPTY: JumpArcLayout = {arcs: [], laneCount: 0};

// Horizontal distance between lanes and the gap between the innermost lane
// and the code.
const LANE_STEP_PX = 8;
const GUTTER_PAD_PX = 6;

// Assigns a lane to every branch whose target is an instruction of the
// listing. Shorter arcs are placed first so an inner loop gets a lower lane
// than the control flow enclosing it.
export function layoutJumpArcs(
  instructions: ReadonlyArray<{
    readonly relPc: bigint;
    readonly targetRelPc?: bigint;
  }>,
): JumpArcLayout {
  const indexByRelPc = new Map<bigint, number>();
  instructions.forEach((insn, i) => indexByRelPc.set(insn.relPc, i));

  const pending: Array<Omit<JumpArc, 'lane'>> = [];
  instructions.forEach((insn, fromIndex) => {
    if (insn.targetRelPc === undefined) return;
    const toIndex = indexByRelPc.get(insn.targetRelPc);
    if (toIndex === undefined || toIndex === fromIndex) return;
    pending.push({fromIndex, toIndex, backEdge: toIndex < fromIndex});
  });
  if (pending.length === 0) return EMPTY;
  pending.sort((a, b) => span(a) - span(b));

  // The row intervals occupied in each lane. An arc takes the lowest lane
  // whose intervals it does not overlap.
  const lanes: Array<Array<readonly [number, number]>> = [];
  const arcs: JumpArc[] = [];
  for (const arc of pending) {
    const lo = Math.min(arc.fromIndex, arc.toIndex);
    const hi = Math.max(arc.fromIndex, arc.toIndex);
    let lane = 0;
    for (;;) {
      const occupied = (lanes[lane] ??= []);
      if (occupied.every(([a, b]) => hi < a || lo > b)) {
        occupied.push([lo, hi]);
        break;
      }
      lane++;
    }
    arcs.push({...arc, lane});
  }
  return {arcs, laneCount: lanes.length};
}

function span(arc: Omit<JumpArc, 'lane'>): number {
  return Math.abs(arc.toIndex - arc.fromIndex);
}

export function jumpGutterWidthPx(layout: JumpArcLayout): number {
  return layout.laneCount === 0
    ? 0
    : layout.laneCount * LANE_STEP_PX + GUTTER_PAD_PX;
}

// The slice of the gutter belonging to one row: the vertical segments of
// the lanes passing through it, and for a branch or target row the
// horizontal connector to the code, with an arrowhead on the target.
export function renderJumpGutterRow(
  rowIndex: number,
  layout: JumpArcLayout,
  rowHeightPx: number,
): m.Children {
  const width = jumpGutterWidthPx(layout);
  if (width === 0) return undefined;
  const mid = rowHeightPx / 2;
  const paths: m.Children[] = [];
  for (const arc of layout.arcs) {
    const lo = Math.min(arc.fromIndex, arc.toIndex);
    const hi = Math.max(arc.fromIndex, arc.toIndex);
    if (rowIndex < lo || rowIndex > hi) continue;
    const x = width - (arc.lane + 1) * LANE_STEP_PX;
    const className = arc.backEdge
      ? 'pf-source-annotation__arc pf-source-annotation__arc--loop'
      : 'pf-source-annotation__arc';
    // Vertical part: full height inside the span, half at either end.
    const y0 = rowIndex === lo ? mid : 0;
    const y1 = rowIndex === hi ? mid : rowHeightPx;
    let d = `M${x},${y0} V${y1}`;
    if (rowIndex === arc.fromIndex || rowIndex === arc.toIndex) {
      d += ` M${x},${mid} H${width}`;
    }
    paths.push(m('path', {className, d}));
    if (rowIndex === arc.toIndex) {
      paths.push(
        m('path', {
          className: className + ' pf-source-annotation__arc-head',
          d: `M${width - 4},${mid - 3} L${width},${mid} L${width - 4},${mid + 3}`,
        }),
      );
    }
  }
  return m(
    'svg.pf-source-annotation__gutter',
    {width, height: rowHeightPx, viewBox: `0 0 ${width} ${rowHeightPx}`},
    paths,
  );
}
