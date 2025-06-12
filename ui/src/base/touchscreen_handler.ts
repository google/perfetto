// Copyright (C) 2025 The Android Open Source Project
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

import {DisposableStack} from './disposable_stack';
import {bindEventListener} from './dom_utils';
import {assertTrue} from './logging';

export interface TouchArgs {
  clientX: number;
  clientY: number;
  deltaX: number;
  deltaY: number;
}

interface TouchHandlerCallbacks {
  onPinchZoom?: (args: TouchArgs) => void;
  onPan?: (args: TouchArgs) => void;
  onTapDown?: (args: TouchArgs) => void;
  onTapMove?: (args: TouchArgs) => void;
  onTapUp?: (args: TouchArgs) => void;
}

const PAN_TIMEOUT_MS = 200;

/**
 * TouchscreenHandler handles 3 types of gestures using Touch APIs on devices
 * that support touch. Note this has nothing to do with "touchpad" (that is
 * handled using wheel event). This has to do with mouse-less devices.
 * This class supports 3 types of gestures:
 * 1. Two finger pinch zoom (it always takes the precedence).
 * 2. Single finger tap.
 * 3. Single finger mouse emulation.
 *
 * 2 and 3 are disamgiguated by time: a touch down followed by a quick move is
 * interpreted like a pan. Instead a touch down followed by a PAN_TIMEOUT_MS
 * pause is interpreted as a mouse-like event.
 */
export class TouchscreenHandler {
  private readonly trash = new DisposableStack();

  private callbacks: TouchHandlerCallbacks;
  private tapStart?: Touch;
  private tapStartTime = 0;
  private tapGeneration = 0;
  private lastTouch?: Touch;
  private lastPinch?: Touch[];
  private mode?: 'deciding' | 'pinch' | 'pan' | 'mouse_emulation';

  constructor(target: HTMLElement, callbacks: TouchHandlerCallbacks) {
    this.callbacks = callbacks;

    this.trash.use(
      bindEventListener(target, 'touchstart', this.onTouchStart.bind(this)),
    );
    this.trash.use(
      bindEventListener(target, 'touchmove', this.onTouchMove.bind(this), {
        passive: false,
      }),
    );
    this.trash.use(
      bindEventListener(target, 'touchend', this.onTouchEnd.bind(this)),
    );
  }

  [Symbol.dispose](): void {
    this.trash.dispose();
  }

  // NOTE: touch events can be overlapped. If we start touching with a single
  // finger and then we see a second one (which is very common by accident
  // because humans rarely manage to put both fingers on screen at the same
  // time), we'll see something like:
  // onTouchStart(1 touch)
  // onTouchStart(2 touches)
  // ...
  // onTouchEnd(2 touches)
  // onTouchEnd(1 touch)
  // We handle this by using the following logic:
  // - pan and mouse_emulation are mutually exclusive, and they are determined
  //   based on what you do within the first 200ms.
  // - you can "upgrade" from a pan or mouse_emulation to pinch if you add a
  //   second finger.
  // - However, once in pinch mode, you can'd downgrade.

  private onTouchStart = (e: TouchEvent) => {
    if (e.touches.length === 2) {
      this.mode = 'pinch';
      return;
    }

    if (e.touches.length !== 1) return;

    const t = e.touches[0];
    this.tapStart = t;
    this.tapStartTime = performance.now();
    this.lastTouch = t;
    this.mode = 'deciding';
    const tapGeneration = ++this.tapGeneration;
    self.setTimeout(() => {
      if (this.tapGeneration === tapGeneration && this.mode === 'deciding') {
        this.initiateMouseEmulation(t);
      }
    }, PAN_TIMEOUT_MS);
  };

  private initiateMouseEmulation(t: {clientX: number; clientY: number}) {
    assertTrue(this.mode === 'deciding');
    this.mode = 'mouse_emulation';
    this.callbacks.onTapDown?.({
      deltaX: 0,
      deltaY: 0,
      clientX: t.clientX,
      clientY: t.clientY,
    });
  }

  private onTouchMove = (e: TouchEvent) => {
    if (e.touches.length < 1 || e.touches.length > 2 || !this.tapStart) {
      return;
    }

    if (e.touches.length === 2) {
      e.preventDefault(); // Disable browser panning.
      if (this.mode === 'mouse_emulation' && this.tapStart !== undefined) {
        this.callbacks.onTapUp?.({
          deltaX: 0,
          deltaY: 0,
          ...this.tapStart,
        });
      }
      this.mode = 'pinch';
      this.handlePinch(e);
      return;
    }

    // When pinch-zooming we can get a mixture of interleaved touchmove(1) and
    // touchmove(2) depending on how many fingers we started with. Suppress the
    // former as it creates jittery vertical scrolling while panning.
    if (this.mode === 'pinch') {
      e.preventDefault();
      return;
    }

    // Single touch case. Here we need to disambiguate between:
    // 1. A pan, which is a tapdown followed by a rapid movement.
    // 2. A mouse-like touchmouse_emulation event, which we trigger when the user
    //    taps down and then doesn't move for TAP_TIMEOUT_MS.
    const t = e.touches[0];
    const dXInit = t.clientX - this.tapStart.clientX;
    const dYInit = t.clientY - this.tapStart.clientY;
    const distSq = dXInit * dXInit + dYInit * dYInit;

    this.lastTouch ||= this.tapStart;
    const deltaX = this.lastTouch.clientX - t.clientX;
    const deltaY = this.lastTouch.clientY - t.clientY;
    const args = {
      clientX: t.clientX,
      clientY: t.clientY,
      deltaX,
      deltaY,
    };
    this.lastTouch = t;
    const elapsed = performance.now() - this.tapStartTime;

    if (this.mode === 'deciding') {
      if (distSq > 25 && elapsed < PAN_TIMEOUT_MS) {
        this.mode = 'pan';
      } else if (elapsed >= PAN_TIMEOUT_MS) {
        this.initiateMouseEmulation(this.tapStart);
      } else {
        return;
      }
    }

    // Not an "else" because we want to fall through after we change the
    // singleTapType above.
    if (this.mode === 'mouse_emulation') {
      e.preventDefault();
      this.callbacks.onTapMove?.({...args});
    } else if (this.mode === 'pan') {
      this.callbacks.onPan?.({...args});
    }
  };

  private onTouchEnd = (e: TouchEvent) => {
    this.tapStart = undefined;
    const mode = this.mode;
    this.mode = undefined;
    this.lastTouch = undefined;
    this.lastPinch = undefined;

    if (mode === 'mouse_emulation') {
      const t = e.changedTouches[0];
      this.callbacks.onTapUp?.({
        deltaX: 0,
        deltaY: 0,
        clientX: t.clientX,
        clientY: t.clientY,
      });
    }
  };

  private handlePinch(e: TouchEvent) {
    if (e.touches.length !== 2) return;

    if (this.lastPinch === undefined) {
      this.lastPinch = [e.touches[0], e.touches[1]];
    }

    function distance(t: ArrayLike<Touch>) {
      const dX = t[0].clientX - t[1].clientX;
      const dY = t[0].clientY - t[1].clientY;
      return Math.sqrt(dX * dX + dY * dY);
    }

    const delta = Math.round(distance(this.lastPinch) - distance(e.touches));
    this.lastPinch = [e.touches[0], e.touches[1]];

    const midX = (e.touches[0].clientX + e.touches[1].clientX) / 2;
    const midY = (e.touches[0].clientY + e.touches[1].clientY) / 2;

    this.callbacks.onPinchZoom?.({
      clientX: Math.round(midX),
      clientY: Math.round(midY),
      deltaX: delta,
      deltaY: delta,
    });
  }
}

export type TouchEventTranslation =
  | 'down-up-move'
  | 'pan-x'
  | 'pinch-zoom-as-ctrl-wheel';
export function convertTouchIntoMouseEvents(
  target: HTMLElement,
  events: TouchEventTranslation[],
): Disposable {
  return new TouchscreenHandler(target, {
    onTapDown(args: TouchArgs) {
      if (!events.includes('down-up-move')) return;
      target.dispatchEvent(
        new MouseEvent('mousedown', {
          bubbles: true,
          buttons: 1,
          ...args,
        }),
      );
    },
    onTapUp(args: TouchArgs) {
      if (!events.includes('down-up-move')) return;
      target.dispatchEvent(
        new MouseEvent('mouseup', {
          bubbles: true,
          buttons: 0,
          ...args,
        }),
      );
    },
    onTapMove(args: TouchArgs) {
      if (!events.includes('down-up-move')) return;
      target.dispatchEvent(
        new MouseEvent('mousemove', {
          bubbles: true,
          buttons: 1,
          movementX: args.deltaX,
          movementY: args.deltaY,
          ...args,
        }),
      );
    },
    onPan(args: TouchArgs) {
      if (
        Math.abs(args.deltaX) > Math.abs(args.deltaY) &&
        events.includes('pan-x')
      ) {
        withInertia(args.deltaX, 0, (dx) =>
          target.dispatchEvent(
            new WheelEvent('wheel', {
              ...args,
              deltaX: dx,
              deltaY: 0,
              ctrlKey: false,
            }),
          ),
        );
      }
    },
    onPinchZoom(args: TouchArgs) {
      if (!events.includes('pinch-zoom-as-ctrl-wheel')) return;
      // We translate pinch zoom into Ctrl+vertical wheel. This is consistent
      // with what most laptops seem to do when pinching on the touchpad.
      target.dispatchEvent(
        new WheelEvent('wheel', {
          ...args,
          deltaX: 0,
          deltaY: args.deltaY,
          ctrlKey: true,
        }),
      );
    },
  });
}

function withInertia(
  vx: number,
  vy: number,
  callback: (dx: number, dy: number) => void,
  options?: {
    friction?: number;
    minVelocity?: number;
  },
): () => void {
  const friction = options?.friction ?? 0.8;
  const minVelocity = options?.minVelocity ?? 0.05;

  let frame: number | null = null;

  function step() {
    vx *= friction;
    vy *= friction;

    if (Math.abs(vx) < minVelocity && Math.abs(vy) < minVelocity) {
      if (frame !== null) cancelAnimationFrame(frame);
      return;
    }

    callback(vx, vy);
    frame = requestAnimationFrame(step);
  }

  frame = requestAnimationFrame(step);

  return () => {
    if (frame !== null) {
      cancelAnimationFrame(frame);
      frame = null;
    }
  };
}
