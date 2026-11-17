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

import {type Bounds2D, Rect2D} from '../../base/geom';
import {OWNER_ATTRIBUTE} from '../../base/owner_attribute';
import {findAttributionForElement, UNATTRIBUTED} from '../ownership';
import {type LabeledRect, PaintedRects} from './painted_rects';
import {VisibleRects} from './visible_rects';

/** A part of the screen and its owner. */
export interface OwnedBox {
  readonly bounds: Bounds2D;
  readonly owner: string;
}

/** A point where the snapshot and the page name different owners. */
export interface PointDisagreement {
  readonly x: number;
  readonly y: number;
  /** The owner of what the browser draws at the point. */
  readonly pageOwner: string;
  /** The owner the snapshot gives the point. */
  readonly snapshotOwner: string;
  /** What the browser draws at the point, as tag#id.class. */
  readonly element: string;
}

export interface PointComparison {
  /** Points checked, leaving out those near a box edge. */
  readonly compared: number;
  readonly disagreements: ReadonlyArray<PointDisagreement>;
}

/**
 * Who owns each part of the screen at one moment.
 *
 * It works in three steps:
 *
 * Step 1: read the page. Find every labeled element and measure the part of
 * it you can see (see VisibleRects).
 *
 * Step 2: paint a flat picture. Paint those boxes outer before inner, so inner
 * labels end up on top. Parts nothing is painted on are unattributed.
 *
 * Step 3: now the caller of ScreenSnapshot.take() can ask the following:
 * - findOwnerAt(x, y): who owns a point;
 * - computeShares(): a mapping of how much of the screen each owner takes up;
 * - listRegions(): the picture as boxes that don't overlap, one owner each;
 * - boxes: the raw boxes read in step 1;
 * - compareWithPage(): where the picture and the page disagree.
 *
 * E.g.
 *   const snapshot = ScreenSnapshot.take();
 *   snapshot.computeShares();   // Map {'dev.perfetto.Timeline' => 412, ...}
 *   snapshot.findOwnerAt(x, y); // 'dev.perfetto.Sched'
 */
export class ScreenSnapshot {
  private readonly painted: PaintedRects;

  /** Takes a snapshot of the page as it is now. */
  static take(): ScreenSnapshot {
    const viewport = new Rect2D({
      left: 0,
      top: 0,
      right: window.innerWidth,
      bottom: window.innerHeight,
    });
    // Step 1: read the owners and boxes from the DOM.
    const boxes: OwnedBox[] = readOwnedBoxes(viewport);
    // Step 2: paint the flat picture with the boxes. This happens in the
    // constructor.
    return new ScreenSnapshot(viewport, boxes);
  }

  /** Makes a snapshot by painting the given boxes. */
  constructor(
    private readonly viewport: Bounds2D,
    readonly boxes: ReadonlyArray<OwnedBox>,
  ) {
    // Paint the whole viewport first, as unattributed, so parts no box covers
    // are unattributed. It must come first, because later rectangles are
    // painted over earlier ones.
    const background: LabeledRect = {bounds: viewport, label: UNATTRIBUTED};
    const labeled: LabeledRect[] = boxes.map((b) => ({
      bounds: b.bounds,
      label: b.owner,
    }));
    this.painted = new PaintedRects([background, ...labeled]);
  }

  /** Finds the owner of the point (x, y). */
  findOwnerAt(x: number, y: number): string {
    return this.painted.findLabelAt(x, y) ?? UNATTRIBUTED;
  }

  /**
   * Lists the flat picture as rectangles that don't overlap, each with the
   * owner that shows there. Unlike `boxes`, which are the labeled elements
   * as-read and overlap each other, these are only what ends up on top.
   */
  listRegions(): OwnedBox[] {
    return this.painted.listRegions().map((r) => ({
      bounds: r.bounds,
      owner: r.label,
    }));
  }

  /**
   * Computes how much of the screen each owner covers, out of 1000. E.g. 412
   * for dev.perfetto.Timeline means the timeline page shows on 41.2% of the
   * screen.
   */
  computeShares(): Map<string, number> {
    const areas = new Map<string, number>();
    for (const {bounds, owner} of this.listRegions()) {
      const area = (bounds.right - bounds.left) * (bounds.bottom - bounds.top);
      areas.set(owner, (areas.get(owner) ?? 0) + area);
    }
    return toThousandths(areas);
  }

  /**
   * Checks the snapshot against the page: looks up a point every `step`
   * pixels both in the snapshot and by asking the browser what is drawn
   * there, and lists where they name different owners. Where they disagree,
   * the snapshot is wrong, for example because something without a label is
   * drawn on top of a labeled box.
   *
   * Points within `edgeBand` pixels of a box edge are skipped: sub-pixel
   * rounding makes either answer right there.
   */
  compareWithPage(step: number, edgeBand: number): PointComparison {
    const {left, top, right, bottom} = this.viewport;
    let compared = 0;
    const disagreements: PointDisagreement[] = [];
    for (let y = top + step / 2; y < bottom; y += step) {
      for (let x = left + step / 2; x < right; x += step) {
        if (this.painted.measureDistanceToEdge(x, y) < edgeBand) continue;
        compared++;
        const snapshotOwner = this.findOwnerAt(x, y);
        const element = document.elementFromPoint(x, y);
        const pageOwner = findAttributionForElement(element).owner;
        if (pageOwner !== snapshotOwner) {
          disagreements.push({
            x,
            y,
            pageOwner,
            snapshotOwner,
            element: describeElement(element),
          });
        }
      }
    }
    return {compared, disagreements};
  }
}

// Describes an element as tag#id.class, for messages.
function describeElement(el: Element | null): string {
  if (el === null) return '(nothing)';
  const id = el.id ? `#${el.id}` : '';
  const classes = Array.from(el.classList)
    .slice(0, 3)
    .map((c) => `.${c}`)
    .join('');
  return `${el.tagName.toLowerCase()}${id}${classes}`;
}

// Returns the visible part of each labeled element on the page, credited to its
// owner, outer elements before inner ones.
function readOwnedBoxes(viewport: Rect2D): OwnedBox[] {
  // Measure every element with the same VisibleRects, so elements in the same
  // scrolling list reuse what it measured about that list.
  const visible = new VisibleRects(viewport);
  // querySelectorAll lists elements in document order, which puts every
  // element before the elements inside it. So inner boxes come later and are
  // painted over outer ones.
  const elements = document.querySelectorAll(`[${OWNER_ATTRIBUTE}]`);
  return Array.from(elements).flatMap((el) => {
    const owner = el.getAttribute(OWNER_ATTRIBUTE) || UNATTRIBUTED;
    return visible.of(el).map((bounds) => ({bounds, owner}));
  });
}

// Converts areas to whole thousandths of their total that add up to 1000,
// giving the rounding leftovers to the largest remainders. Leaves out areas
// that round to 0, and returns nothing if the total is 0.
function toThousandths(
  areas: ReadonlyMap<string, number>,
): Map<string, number> {
  let total = 0;
  for (const a of areas.values()) total += a;
  if (total <= 0) return new Map();
  const items = Array.from(areas.entries()).map(([owner, area]) => {
    const exact = (area * 1000) / total;
    const whole = Math.floor(exact);
    return {owner, whole, rest: exact - whole};
  });
  let left = 1000 - items.reduce((sum, i) => sum + i.whole, 0);
  items.sort((a, b) => b.rest - a.rest);
  for (const i of items) {
    if (left <= 0) break;
    i.whole++;
    left--;
  }
  return new Map(
    items.filter((i) => i.whole > 0).map((i) => [i.owner, i.whole]),
  );
}
