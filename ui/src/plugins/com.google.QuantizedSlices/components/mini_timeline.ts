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

import m from 'mithril';
import {MergedSlice} from '../models/types';
import {renderMiniCanvas, showTooltip, hideTooltip} from './timeline_canvas';
import type {HitRect} from './timeline_canvas';

/**
 * Minimal interface for the trace-level state that MiniTimeline needs.
 * Avoids a direct import of the plugin's global state module, preventing
 * circular dependencies.
 */
export interface MiniTimelineTrace {
  currentSeq: MergedSlice[];
  totalDur: number;
}

interface MiniTimelineAttrs {
  ts: MiniTimelineTrace;
  /** Optional callback invoked before rendering to ensure caches are warm. */
  ensureCache?: (ts: MiniTimelineTrace) => void;
}

// Store hit rects + trace state per canvas — survives across lifecycle hooks.
const canvasHits = new WeakMap<
  HTMLCanvasElement,
  {hits: HitRect[]; totalDur: number; ts: MiniTimelineTrace}
>();
const canvasHover = new WeakMap<HTMLCanvasElement, number | undefined>();
type CanvasListeners = {
  move: (e: MouseEvent) => void;
  leave: (e: MouseEvent) => void;
};
const canvasListeners = new WeakMap<HTMLCanvasElement, CanvasListeners>();

// Viewport-gated rendering via IntersectionObserver.
const isVisible = new WeakMap<Element, boolean>();
const needsRender = new WeakSet<Element>();

let observer: IntersectionObserver | null = null;
let observerRefCount = 0;
const hasIO = typeof IntersectionObserver !== 'undefined';

function getObserver(): IntersectionObserver | null {
  if (!hasIO) return null;
  if (!observer) {
    observer = new IntersectionObserver(
      (entries) => {
        let anyBecameVisible = false;
        for (const entry of entries) {
          const wasVisible = isVisible.get(entry.target) ?? false;
          isVisible.set(entry.target, entry.isIntersecting);
          if (
            entry.isIntersecting &&
            !wasVisible &&
            needsRender.has(entry.target)
          ) {
            needsRender.delete(entry.target);
            anyBecameVisible = true;
          }
        }
        if (anyBecameVisible) m.redraw();
      },
      {rootMargin: '200px 0px'},
    );
  }
  observerRefCount++;
  return observer;
}

function releaseObserver(): void {
  if (!hasIO) return;
  observerRefCount--;
  if (observerRefCount <= 0 && observer) {
    observer.disconnect();
    observer = null;
    observerRefCount = 0;
  }
}

function doRender(
  dom: Element,
  ts: MiniTimelineTrace,
  ensureCache?: (ts: MiniTimelineTrace) => void,
): void {
  if (hasIO && !isVisible.get(dom)) {
    needsRender.add(dom);
    return;
  }
  needsRender.delete(dom);
  const canvas = dom.querySelector('canvas');
  if (!canvas) return;
  if (ensureCache) ensureCache(ts);
  const hits = renderMiniCanvas(canvas, {
    seq: ts.currentSeq,
    totalDur: ts.totalDur,
    highlightIdx: canvasHover.get(canvas),
  });
  canvasHits.set(canvas, {hits, totalDur: ts.totalDur, ts});
}

export const MiniTimeline: m.Component<MiniTimelineAttrs> = {
  oncreate(vnode: m.VnodeDOM<MiniTimelineAttrs>) {
    const obs = getObserver();
    if (obs) {
      needsRender.add(vnode.dom);
      obs.observe(vnode.dom);
    } else {
      doRender(vnode.dom, vnode.attrs.ts, vnode.attrs.ensureCache);
    }

    const canvas = vnode.dom.querySelector('canvas');
    if (!canvas) return;

    const onMove = (e: MouseEvent): void => {
      const cvs = e.target as HTMLCanvasElement;
      const data = canvasHits.get(cvs);
      if (!data) return;
      showTooltip(e, data.hits, data.totalDur);

      // Find hovered segment index and re-render with highlight.
      const rect = cvs.getBoundingClientRect();
      const mx = e.clientX - rect.left;
      const my = e.clientY - rect.top;
      let hitIdx: number | undefined;
      for (let i = data.hits.length - 1; i >= 0; i--) {
        const r = data.hits[i];
        if (mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h) {
          hitIdx = i;
          break;
        }
      }
      if (hitIdx !== canvasHover.get(cvs)) {
        canvasHover.set(cvs, hitIdx);
        const hits = renderMiniCanvas(cvs, {
          seq: data.ts.currentSeq,
          totalDur: data.totalDur,
          highlightIdx: hitIdx,
        });
        canvasHits.set(cvs, {hits, totalDur: data.totalDur, ts: data.ts});
      }
    };

    const onLeave = (_e: MouseEvent): void => {
      hideTooltip();
      const cvs = _e.target as HTMLCanvasElement;
      const data = canvasHits.get(cvs);
      if (data && canvasHover.get(cvs) != null) {
        canvasHover.delete(cvs);
        const hits = renderMiniCanvas(cvs, {
          seq: data.ts.currentSeq,
          totalDur: data.totalDur,
        });
        canvasHits.set(cvs, {hits, totalDur: data.totalDur, ts: data.ts});
      }
    };

    canvas.addEventListener('mousemove', onMove);
    canvas.addEventListener('mouseleave', onLeave);
    canvasListeners.set(canvas, {move: onMove, leave: onLeave});
  },

  onupdate(vnode: m.VnodeDOM<MiniTimelineAttrs>) {
    doRender(vnode.dom, vnode.attrs.ts, vnode.attrs.ensureCache);
  },

  onremove(vnode: m.VnodeDOM<MiniTimelineAttrs>) {
    const canvas = vnode.dom.querySelector('canvas');
    if (canvas) {
      const listeners = canvasListeners.get(canvas);
      if (listeners) {
        canvas.removeEventListener('mousemove', listeners.move);
        canvas.removeEventListener('mouseleave', listeners.leave);
        canvasListeners.delete(canvas);
      }
    }
    if (observer) observer.unobserve(vnode.dom);
    isVisible.delete(vnode.dom);
    needsRender.delete(vnode.dom);
    releaseObserver();
  },

  view() {
    return m('.qs-mini-canvas', m('canvas'));
  },
};
