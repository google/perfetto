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

import {DisposableStack} from '../../base/disposable_stack';
import {currentTargetOffset, elementIsEditable} from '../../base/dom_utils';
import {KeyMapping} from '../../base/wasd_key_mapping';

// A minimal spring-step driver on requestAnimationFrame — a local equivalent of
// frontend/animation's Animation (which the import checker won't let a plugin
// use, as it lives in /frontend). Calls onStep(msSinceStart) each frame until
// the duration elapses; start() while running just extends the end time.
class Animation {
  private startMs = 0;
  private endMs = 0;
  private rafId = 0;
  private running = false;

  constructor(private readonly onStep: (msSinceStart: number) => void) {}

  private readonly frame = (nowMs: number) => {
    if (!this.running) return;
    if (nowMs >= this.endMs) {
      this.running = false;
      return;
    }
    this.onStep(Math.max(Math.round(nowMs - this.startMs), 0));
    this.rafId = requestAnimationFrame(this.frame);
  };

  start(durationMs: number) {
    const nowMs = performance.now();
    this.endMs = nowMs + durationMs;
    if (!this.running) {
      this.startMs = nowMs;
      this.running = true;
      this.rafId = requestAnimationFrame(this.frame);
    }
  }

  stop() {
    this.endMs = 0;
    this.running = false;
    cancelAnimationFrame(this.rafId);
  }
}

// Self-contained WASD horizontal pan/zoom, matching the main timeline's feel
// (same constants and spring animation). A/D pan, W/S zoom towards the cursor.
// It is a trimmed copy of the timeline's KeyboardNavigationHandler kept local so
// this plugin does not depend on another plugin's internals.
const INITIAL_PAN_STEP_PX = 50;
const INITIAL_ZOOM_STEP = 0.1;
const SNAP_FACTOR = 0.4;
const ACCELERATION_PER_MS = 1 / 50;
const DEFAULT_ANIMATION_DURATION = 700;
const ZOOM_RATIO_PER_FRAME = 0.008;
const KEYBOARD_PAN_PX_PER_FRAME = 8;

enum Pan {
  None = 0,
  Left = -1,
  Right = 1,
}
enum Zoom {
  None = 0,
  In = 1,
  Out = -1,
}

function keyToPan(e: KeyboardEvent): Pan {
  if (e.code === KeyMapping.KEY_PAN_LEFT) return Pan.Left;
  if (e.code === KeyMapping.KEY_PAN_RIGHT) return Pan.Right;
  return Pan.None;
}
function keyToZoom(e: KeyboardEvent): Zoom {
  if (e.code === KeyMapping.KEY_ZOOM_IN) return Zoom.In;
  if (e.code === KeyMapping.KEY_ZOOM_OUT) return Zoom.Out;
  return Zoom.None;
}

export interface WasdNavigationArgs {
  readonly element: HTMLElement;
  // Pan the view by `movedPx` pixels (positive = towards later time).
  readonly onPanned: (movedPx: number) => void;
  // Zoom by `zoomRatio` centred on element-x `zoomPositionPx`.
  readonly onZoomed: (zoomPositionPx: number, zoomRatio: number) => void;
}

export class WasdNavigation implements Disposable {
  private mouseX: number | undefined;
  private panning = Pan.None;
  private panOffsetPx = 0;
  private targetPanOffsetPx = 0;
  private zooming = Zoom.None;
  private zoomRatio = 0;
  private targetZoomRatio = 0;
  private readonly panAnimation = new Animation((ms) => this.onPanStep(ms));
  private readonly zoomAnimation = new Animation((ms) => this.onZoomStep(ms));
  private readonly trash = new DisposableStack();
  private readonly args: WasdNavigationArgs;
  private disposed = false;

  constructor(args: WasdNavigationArgs) {
    this.args = args;
    const onKeyDown = (e: Event) => this.onKeyDown(e);
    const onKeyUp = (e: Event) => this.onKeyUp(e);
    const onMouseMove = (e: MouseEvent) => {
      this.mouseX = currentTargetOffset(e).x;
    };
    document.body.addEventListener('keydown', onKeyDown);
    document.body.addEventListener('keyup', onKeyUp);
    args.element.addEventListener('mousemove', onMouseMove);
    this.trash.defer(() => {
      document.body.removeEventListener('keydown', onKeyDown);
      document.body.removeEventListener('keyup', onKeyUp);
      args.element.removeEventListener('mousemove', onMouseMove);
    });
  }

  [Symbol.dispose]() {
    if (this.disposed) return;
    this.disposed = true;
    this.panAnimation.stop();
    this.zoomAnimation.stop();
    this.trash.dispose();
  }

  private onPanStep(msSinceStart: number) {
    const step = (this.targetPanOffsetPx - this.panOffsetPx) * SNAP_FACTOR;
    if (this.panning !== Pan.None) {
      const velocity = 1 + msSinceStart * ACCELERATION_PER_MS;
      this.targetPanOffsetPx +=
        this.panning * Math.max(KEYBOARD_PAN_PX_PER_FRAME * velocity, step);
    }
    this.panOffsetPx += step;
    if (Math.abs(step) > 1e-1) {
      this.args.onPanned(step);
    } else {
      this.panAnimation.stop();
    }
  }

  private onZoomStep(msSinceStart: number) {
    if (this.mouseX === undefined) return;
    const step = (this.targetZoomRatio - this.zoomRatio) * SNAP_FACTOR;
    if (this.zooming !== Zoom.None) {
      const velocity = 1 + msSinceStart * ACCELERATION_PER_MS;
      this.targetZoomRatio +=
        this.zooming * Math.max(ZOOM_RATIO_PER_FRAME * velocity, step);
    }
    this.zoomRatio += step;
    if (Math.abs(step) > 1e-6) {
      this.args.onZoomed(this.mouseX, step);
    } else {
      this.zoomAnimation.stop();
    }
  }

  private onKeyDown(e: Event) {
    if (!(e instanceof KeyboardEvent)) return;
    if (elementIsEditable(e.target)) return;
    if (e.ctrlKey || e.metaKey) return;

    const pan = keyToPan(e);
    if (pan !== Pan.None) {
      if (this.panning !== pan) {
        this.panAnimation.stop();
        this.panOffsetPx = 0;
        this.targetPanOffsetPx = pan * INITIAL_PAN_STEP_PX;
      }
      this.panning = pan;
      this.panAnimation.start(DEFAULT_ANIMATION_DURATION);
    }

    const zoom = keyToZoom(e);
    if (zoom !== Zoom.None) {
      if (this.zooming !== zoom) {
        this.zoomAnimation.stop();
        this.zoomRatio = 0;
        this.targetZoomRatio = zoom * INITIAL_ZOOM_STEP;
      }
      this.zooming = zoom;
      this.zoomAnimation.start(DEFAULT_ANIMATION_DURATION);
    }
  }

  private onKeyUp(e: Event) {
    if (!(e instanceof KeyboardEvent)) return;
    if (keyToPan(e) !== Pan.None) this.panning = Pan.None;
    if (keyToZoom(e) !== Zoom.None) this.zooming = Zoom.None;
  }
}
