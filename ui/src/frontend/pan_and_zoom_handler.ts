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

import {Animation} from './animation';
import Timer = NodeJS.Timer;
import {DragGestureHandler} from './drag_gesture_handler';
import {globals} from './globals';

const ZOOM_RATIO_PER_FRAME = 0.008;
const KEYBOARD_PAN_PX_PER_FRAME = 8;
const HORIZONTAL_WHEEL_PAN_SPEED = 1;
const WHEEL_ZOOM_SPEED = -0.02;

// Usually, animations are cancelled on keyup. However, in case the keyup
// event is not captured by the document, e.g. if it loses focus first, then
// we want to stop the animation as soon as possible.
const ANIMATION_AUTO_END_AFTER_INITIAL_KEYPRESS_MS = 700;
const ANIMATION_AUTO_END_AFTER_KEYPRESS_MS = 80;

// This defines the step size for an individual pan or zoom keyboard tap.
const TAP_ANIMATION_TIME = 200;

enum Pan {
  None = 0,
  Left = -1,
  Right = 1
}
function keyToPan(e: KeyboardEvent): Pan {
  if (['a'].includes(e.key)) return Pan.Left;
  if (['d'].includes(e.key)) return Pan.Right;
  return Pan.None;
}

enum Zoom {
  None = 0,
  In = 1,
  Out = -1
}
function keyToZoom(e: KeyboardEvent): Zoom {
  if (['w'].includes(e.key)) return Zoom.In;
  if (['s'].includes(e.key)) return Zoom.Out;
  return Zoom.None;
}

/**
 * Enables horizontal pan and zoom with mouse-based drag and WASD navigation.
 */
export class PanAndZoomHandler {
  private mousePositionX: number|null = null;
  private boundOnMouseMove = this.onMouseMove.bind(this);
  private boundOnWheel = this.onWheel.bind(this);
  private boundOnKeyDown = this.onKeyDown.bind(this);
  private boundOnKeyUp = this.onKeyUp.bind(this);
  private panning: Pan = Pan.None;
  private zooming: Zoom = Zoom.None;
  private cancelPanTimeout?: Timer;
  private cancelZoomTimeout?: Timer;
  private panAnimation = new Animation(this.onPanAnimationStep.bind(this));
  private zoomAnimation = new Animation(this.onZoomAnimationStep.bind(this));

  private element: HTMLElement;
  private contentOffsetX: number;
  private onPanned: (movedPx: number) => void;
  private onZoomed: (zoomPositionPx: number, zoomRatio: number) => void;

  constructor({element, contentOffsetX, onPanned, onZoomed}: {
    element: HTMLElement,
    contentOffsetX: number,
    onPanned: (movedPx: number) => void,
    onZoomed: (zoomPositionPx: number, zoomRatio: number) => void,
  }) {
    this.element = element;
    this.contentOffsetX = contentOffsetX;
    this.onPanned = onPanned;
    this.onZoomed = onZoomed;

    document.body.addEventListener('keydown', this.boundOnKeyDown);
    document.body.addEventListener('keyup', this.boundOnKeyUp);
    this.element.addEventListener('mousemove', this.boundOnMouseMove);
    this.element.addEventListener('wheel', this.boundOnWheel, {passive: true});

    let lastX = -1;
    new DragGestureHandler(this.element, x => {
      this.onPanned(lastX - x);
      lastX = x;
    }, x => lastX = x);
  }

  shutdown() {
    document.body.removeEventListener('keydown', this.boundOnKeyDown);
    document.body.removeEventListener('keyup', this.boundOnKeyUp);
    this.element.removeEventListener('mousemove', this.boundOnMouseMove);
    this.element.removeEventListener('wheel', this.boundOnWheel);
  }

  private onPanAnimationStep(msSinceStartOfAnimation: number) {
    if (this.panning === Pan.None) return;
    let offset = this.panning * KEYBOARD_PAN_PX_PER_FRAME;
    offset *= Math.max(msSinceStartOfAnimation / 40, 1);
    this.onPanned(offset);
  }

  private onZoomAnimationStep(msSinceStartOfAnimation: number) {
    if (this.zooming === Zoom.None || this.mousePositionX === null) return;
    let zoomRatio = this.zooming * ZOOM_RATIO_PER_FRAME;
    zoomRatio *= Math.max(msSinceStartOfAnimation / 40, 1);
    this.onZoomed(this.mousePositionX, zoomRatio);
  }

  private onMouseMove(e: MouseEvent) {
    this.mousePositionX = e.clientX - this.contentOffsetX;
  }

  private onWheel(e: WheelEvent) {
    if (Math.abs(e.deltaX) > Math.abs(e.deltaY)) {
      this.onPanned(e.deltaX * HORIZONTAL_WHEEL_PAN_SPEED);
      globals.rafScheduler.scheduleRedraw();
    } else if (e.ctrlKey && this.mousePositionX) {
      const sign = e.deltaY < 0 ? -1 : 1;
      const deltaY = sign * Math.log2(1 + Math.abs(e.deltaY));
      this.onZoomed(this.mousePositionX, deltaY * WHEEL_ZOOM_SPEED);
      globals.rafScheduler.scheduleRedraw();
    }
  }

  private onKeyDown(e: KeyboardEvent) {
    if (keyToPan(e) !== Pan.None) {
      this.panning = keyToPan(e);
      const animationTime = e.repeat ?
          ANIMATION_AUTO_END_AFTER_KEYPRESS_MS :
          ANIMATION_AUTO_END_AFTER_INITIAL_KEYPRESS_MS;
      this.panAnimation.start(animationTime);
      clearTimeout(this.cancelPanTimeout!);
    }

    if (keyToZoom(e) !== Zoom.None) {
      this.zooming = keyToZoom(e);
      const animationTime = e.repeat ?
          ANIMATION_AUTO_END_AFTER_KEYPRESS_MS :
          ANIMATION_AUTO_END_AFTER_INITIAL_KEYPRESS_MS;
      this.zoomAnimation.start(animationTime);
      clearTimeout(this.cancelZoomTimeout!);
    }
  }

  private onKeyUp(e: KeyboardEvent) {
    if (keyToPan(e) === this.panning) {
      const minEndTime = this.panAnimation.startTimeMs + TAP_ANIMATION_TIME;
      const t = minEndTime - performance.now();
      this.cancelPanTimeout = setTimeout(() => this.panAnimation.stop(), t);
    }
    if (keyToZoom(e) === this.zooming) {
      const minEndTime = this.zoomAnimation.startTimeMs + TAP_ANIMATION_TIME;
      const t = minEndTime - performance.now();
      this.cancelZoomTimeout = setTimeout(() => this.zoomAnimation.stop(), t);
    }
  }
}
