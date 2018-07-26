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

const ZOOM_IN_PERCENTAGE_PER_MS = 0.998;
const ZOOM_OUT_PERCENTAGE_PER_MS = 1 / ZOOM_IN_PERCENTAGE_PER_MS;
const KEYBOARD_PAN_PX_PER_MS = 1;
const HORIZONTAL_WHEEL_PAN_SPEED = 1;

// Usually, animations are cancelled on keyup. However, in case the keyup
// event is not captured by the document, e.g. if it loses focus first, then
// we want to stop the animation as soon as possible.
const ANIMATION_AUTO_END_AFTER_INITIAL_KEYPRESS_MS = 700;
const ANIMATION_AUTO_END_AFTER_KEYPRESS_MS = 80;

// This defines the step size for an individual pan or zoom keyboard tap.
const TAP_ANIMATION_TIME = 200;

const PAN_LEFT_KEYS = ['a'];
const PAN_RIGHT_KEYS = ['d'];
const PAN_KEYS = PAN_LEFT_KEYS.concat(PAN_RIGHT_KEYS);
const ZOOM_IN_KEYS = ['w'];
const ZOOM_OUT_KEYS = ['s'];
const ZOOM_KEYS = ZOOM_IN_KEYS.concat(ZOOM_OUT_KEYS);

/**
 * Enables horizontal pan and zoom with mouse-based drag and WASD navigation.
 */
export class PanAndZoomHandler {
  private mouseDownPositionX: number|null = null;
  private mousePositionX: number|null = null;

  private boundOnMouseDown = this.onMouseDown.bind(this);
  private boundOnMouseMove = this.onMouseMove.bind(this);
  private boundOnMouseUp = this.onMouseUp.bind(this);
  private boundOnWheel = this.onWheel.bind(this);

  private boundOnPanKeyDown: (e: KeyboardEvent) => void = () => {};
  private boundOnPanKeyUp: (e: KeyboardEvent) => void = () => {};
  private boundOnZoomKeyDown: (e: KeyboardEvent) => void = () => {};
  private boundOnZoomKeyUp: (e: KeyboardEvent) => void = () => {};

  private element: HTMLElement;
  private contentOffsetX: number;
  private onPanned: (movedPx: number) => void;
  private onZoomed: (zoomPositionPx: number, zoomPercentage: number) => void;

  constructor({element, contentOffsetX, onPanned, onZoomed}: {
    element: HTMLElement,
    contentOffsetX: number,
    onPanned: (movedPx: number) => void,
    onZoomed: (zoomPositionPx: number, zoomPercentage: number) => void,
  }) {
    this.element = element;
    this.contentOffsetX = contentOffsetX;
    this.onPanned = onPanned;
    this.onZoomed = onZoomed;

    this.element.addEventListener('mousedown', this.boundOnMouseDown);
    this.element.addEventListener('mousemove', this.boundOnMouseMove);
    this.element.addEventListener('mouseup', this.boundOnMouseUp);
    this.element.addEventListener('wheel', this.boundOnWheel, {passive: true});

    this.handleKeyPanning();
    this.handleKeyZooming();
  }

  shutdown() {
    this.element.removeEventListener('mousedown', this.boundOnMouseDown);
    this.element.removeEventListener('mousemove', this.boundOnMouseMove);
    this.element.removeEventListener('mouseup', this.boundOnMouseUp);
    this.element.removeEventListener('wheel', this.boundOnWheel);

    document.body.removeEventListener('keydown', this.boundOnPanKeyDown);
    document.body.removeEventListener('keyup', this.boundOnPanKeyUp);
    document.body.removeEventListener('keydown', this.boundOnZoomKeyDown);
    document.body.removeEventListener('keyup', this.boundOnZoomKeyUp);
  }

  private handleKeyPanning() {
    let directionFactor = 0;
    let tapCancelTimeout: Timer;

    const panAnimation = new Animation((timeSinceLastMs) => {
      this.onPanned(directionFactor * KEYBOARD_PAN_PX_PER_MS * timeSinceLastMs);
    });

    this.boundOnPanKeyDown = e => {
      if (!PAN_KEYS.includes(e.key)) {
        return;
      }
      directionFactor = PAN_LEFT_KEYS.includes(e.key) ? -1 : 1;
      const animationTime = e.repeat ?
          ANIMATION_AUTO_END_AFTER_KEYPRESS_MS :
          ANIMATION_AUTO_END_AFTER_INITIAL_KEYPRESS_MS;
      panAnimation.start(animationTime);
      clearTimeout(tapCancelTimeout);
    };
    this.boundOnPanKeyUp = e => {
      if (!PAN_KEYS.includes(e.key)) {
        return;
      }
      const cancellingDirectionFactor = PAN_LEFT_KEYS.includes(e.key) ? -1 : 1;

      // Only cancel if the lifted key is the one controlling the animation.
      if (cancellingDirectionFactor === directionFactor) {
        const minEndTime = panAnimation.getStartTimeMs() + TAP_ANIMATION_TIME;
        const waitTime = minEndTime - Date.now();
        tapCancelTimeout = setTimeout(() => panAnimation.stop(), waitTime);
      }
    };

    document.body.addEventListener('keydown', this.boundOnPanKeyDown);
    document.body.addEventListener('keyup', this.boundOnPanKeyUp);
  }

  private handleKeyZooming() {
    let zoomingIn = true;
    let tapCancelTimeout: Timer;

    const zoomAnimation = new Animation((timeSinceLastMs: number) => {
      if (this.mousePositionX === null) {
        return;
      }
      const percentagePerMs =
          zoomingIn ? ZOOM_IN_PERCENTAGE_PER_MS : ZOOM_OUT_PERCENTAGE_PER_MS;
      const percentage = Math.pow(percentagePerMs, timeSinceLastMs);
      this.onZoomed(this.mousePositionX, percentage);
    });

    this.boundOnZoomKeyDown = e => {
      if (!ZOOM_KEYS.includes(e.key)) {
        return;
      }
      zoomingIn = ZOOM_IN_KEYS.includes(e.key);
      const animationTime = e.repeat ?
          ANIMATION_AUTO_END_AFTER_KEYPRESS_MS :
          ANIMATION_AUTO_END_AFTER_INITIAL_KEYPRESS_MS;
      zoomAnimation.start(animationTime);
      clearTimeout(tapCancelTimeout);
    };
    this.boundOnZoomKeyUp = e => {
      if (ZOOM_KEYS.includes(e.key)) {
        return;
      }
      const cancellingZoomIn = ZOOM_IN_KEYS.includes(e.key);

      // Only cancel if the lifted key is the one controlling the animation.
      if (cancellingZoomIn === zoomingIn) {
        const minEndTime = zoomAnimation.getStartTimeMs() + TAP_ANIMATION_TIME;
        const waitTime = minEndTime - Date.now();
        tapCancelTimeout = setTimeout(() => zoomAnimation.stop(), waitTime);
      }
    };

    document.body.addEventListener('keydown', this.boundOnZoomKeyDown);
    document.body.addEventListener('keyup', this.boundOnZoomKeyUp);
  }

  private onMouseDown(e: MouseEvent) {
    this.mouseDownPositionX = this.getMouseX(e);
  }

  private onMouseUp() {
    this.mouseDownPositionX = null;
  }

  private onMouseMove(e: MouseEvent) {
    if (this.mouseDownPositionX !== null) {
      this.onPanned(this.mouseDownPositionX - this.getMouseX(e));
      this.mouseDownPositionX = this.getMouseX(e);
      e.preventDefault();
    }
    this.mousePositionX = this.getMouseX(e);
  }

  private onWheel(e: WheelEvent) {
    if (e.deltaX) {
      this.onPanned(e.deltaX * HORIZONTAL_WHEEL_PAN_SPEED);
    }
  }

  private getMouseX(e: MouseEvent) {
    return e.clientX - this.contentOffsetX;
  }
}