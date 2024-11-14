// Copyright (C) 2018 The Android Open Source Project
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

import {PerfStats} from './perf_stats';

export type AnimationCallback = (lastFrameMs: number) => void;
export type RedrawCallback = () => void;

// This class orchestrates all RAFs in the UI. It ensures that there is only
// one animation frame handler overall and that callbacks are called in
// predictable order. There are two types of callbacks here:
// - actions (e.g. pan/zoon animations), which will alter the "fast"
//  (main-thread-only) state (e.g. update visible time bounds @ 60 fps).
// - redraw callbacks that will repaint canvases.
// This class guarantees that, on each frame, redraw callbacks are called after
// all action callbacks.
export class RafScheduler {
  // These happen at the beginning of any animation frame. Used by Animation.
  private animationCallbacks = new Set<AnimationCallback>();

  // These happen during any animaton frame, after the (optional) DOM redraw.
  private canvasRedrawCallbacks = new Set<RedrawCallback>();

  // These happen at the end of full (DOM) animation frames.
  private postRedrawCallbacks = new Array<RedrawCallback>();
  private syncDomRedrawFn: () => void = () => {};
  private hasScheduledNextFrame = false;
  private requestedFullRedraw = false;
  private isRedrawing = false;
  private _shutdown = false;
  private recordPerfStats = false;

  readonly perfStats = {
    rafActions: new PerfStats(),
    rafCanvas: new PerfStats(),
    rafDom: new PerfStats(),
    rafTotal: new PerfStats(),
    domRedraw: new PerfStats(),
  };

  // Called by frontend/index.ts. syncDomRedrawFn is a function that invokes
  // m.render() of the root UiMain component.
  initialize(syncDomRedrawFn: () => void) {
    this.syncDomRedrawFn = syncDomRedrawFn;
  }

  // Schedule re-rendering of virtual DOM and canvas.
  // If a callback is passed it will be executed after the DOM redraw has
  // completed.
  scheduleFullRedraw(cb?: RedrawCallback) {
    this.requestedFullRedraw = true;
    cb && this.postRedrawCallbacks.push(cb);
    this.maybeScheduleAnimationFrame(true);
  }

  // Schedule re-rendering of canvas only.
  scheduleCanvasRedraw() {
    this.maybeScheduleAnimationFrame(true);
  }

  startAnimation(cb: AnimationCallback) {
    this.animationCallbacks.add(cb);
    this.maybeScheduleAnimationFrame();
  }

  stopAnimation(cb: AnimationCallback) {
    this.animationCallbacks.delete(cb);
  }

  addCanvasRedrawCallback(cb: RedrawCallback): Disposable {
    this.canvasRedrawCallbacks.add(cb);
    const canvasRedrawCallbacks = this.canvasRedrawCallbacks;
    return {
      [Symbol.dispose]() {
        canvasRedrawCallbacks.delete(cb);
      },
    };
  }

  shutdown() {
    this._shutdown = true;
  }

  setPerfStatsEnabled(enabled: boolean) {
    this.recordPerfStats = enabled;
    this.scheduleFullRedraw();
  }

  get hasPendingRedraws(): boolean {
    return this.isRedrawing || this.hasScheduledNextFrame;
  }

  private syncDomRedraw() {
    const redrawStart = performance.now();
    this.syncDomRedrawFn();
    if (this.recordPerfStats) {
      this.perfStats.domRedraw.addValue(performance.now() - redrawStart);
    }
  }

  private syncCanvasRedraw() {
    const redrawStart = performance.now();
    if (this.isRedrawing) return;
    this.isRedrawing = true;
    this.canvasRedrawCallbacks.forEach((cb) => cb());
    this.isRedrawing = false;
    if (this.recordPerfStats) {
      this.perfStats.rafCanvas.addValue(performance.now() - redrawStart);
    }
  }

  private maybeScheduleAnimationFrame(force = false) {
    if (this.hasScheduledNextFrame) return;
    if (this.animationCallbacks.size !== 0 || force) {
      this.hasScheduledNextFrame = true;
      window.requestAnimationFrame(this.onAnimationFrame.bind(this));
    }
  }

  private onAnimationFrame(lastFrameMs: number) {
    if (this._shutdown) return;
    this.hasScheduledNextFrame = false;
    const doFullRedraw = this.requestedFullRedraw;
    this.requestedFullRedraw = false;

    const tStart = performance.now();
    this.animationCallbacks.forEach((cb) => cb(lastFrameMs));
    const tAnim = performance.now();
    doFullRedraw && this.syncDomRedraw();
    const tDom = performance.now();
    this.syncCanvasRedraw();
    const tCanvas = performance.now();

    const animTime = tAnim - tStart;
    const domTime = tDom - tAnim;
    const canvasTime = tCanvas - tDom;
    const totalTime = tCanvas - tStart;
    this.updatePerfStats(animTime, domTime, canvasTime, totalTime);
    this.maybeScheduleAnimationFrame();

    if (doFullRedraw && this.postRedrawCallbacks.length > 0) {
      const pendingCbs = this.postRedrawCallbacks.splice(0); // splice = clear.
      pendingCbs.forEach((cb) => cb());
    }
  }

  private updatePerfStats(
    actionsTime: number,
    domTime: number,
    canvasTime: number,
    totalRafTime: number,
  ) {
    if (!this.recordPerfStats) return;
    this.perfStats.rafActions.addValue(actionsTime);
    this.perfStats.rafDom.addValue(domTime);
    this.perfStats.rafCanvas.addValue(canvasTime);
    this.perfStats.rafTotal.addValue(totalRafTime);
  }
}

export const raf = new RafScheduler();
