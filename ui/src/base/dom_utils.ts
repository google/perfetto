// Copyright (C) 2023 The Android Open Source Project
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

import {Vector2D} from './geom';

export type CSSCursor =
  | 'alias'
  | 'all-scroll'
  | 'auto'
  | 'cell'
  | 'context-menu'
  | 'col-resize'
  | 'copy'
  | 'crosshair'
  | 'default'
  | 'e-resize'
  | 'ew-resize'
  | 'grab'
  | 'grabbing'
  | 'help'
  | 'move'
  | 'n-resize'
  | 'ne-resize'
  | 'nesw-resize'
  | 'ns-resize'
  | 'nw-resize'
  | 'nwse-resize'
  | 'no-drop'
  | 'none'
  | 'not-allowed'
  | 'pointer'
  | 'progress'
  | 'row-resize'
  | 's-resize'
  | 'se-resize'
  | 'sw-resize'
  | 'text'
  | 'vertical-text'
  | 'w-resize'
  | 'wait'
  | 'zoom-in'
  | 'zoom-out';

// Check whether a DOM element contains another, or whether they're the same
export function isOrContains(container: Element, target: Element): boolean {
  return container === target || container.contains(target);
}

// Find a DOM element with a given "ref" attribute
export function findRef(root: Element, ref: string): Element | null {
  const query = `[ref=${ref}]`;
  if (root.matches(query)) {
    return root;
  } else {
    return root.querySelector(query);
  }
}

// Safely cast an Element to an HTMLElement.
// Throws if the element is not an HTMLElement.
export function toHTMLElement(el: Element): HTMLElement {
  if (!(el instanceof HTMLElement)) {
    throw new Error('Element is not an HTMLElement');
  }
  return el as HTMLElement;
}

// Return true if EventTarget is or is inside an editable element.
// Editable elements incluce: <input type="text">, <textarea>, or elements with
// the |contenteditable| attribute set.
export function elementIsEditable(target: EventTarget | null): boolean {
  if (target === null) {
    return false;
  }

  if (!(target instanceof Element)) {
    return false;
  }

  const editable = target.closest('input, textarea, [contenteditable=true]');

  if (editable === null) {
    return false;
  }

  if (editable instanceof HTMLInputElement) {
    if (['radio', 'checkbox', 'button'].includes(editable.type)) {
      return false;
    }
  }

  return true;
}

// Returns the mouse pointer's position relative to |e.currentTarget| for a
// given |MouseEvent|.
// Similar to |offsetX|, |offsetY| but for |currentTarget| rather than |target|.
// If the event has no currentTarget or it is not an element, offsetX & offsetY
// are returned instead.
export function currentTargetOffset(e: MouseEvent): Vector2D {
  if (e.currentTarget === e.target) {
    return new Vector2D({x: e.offsetX, y: e.offsetY});
  }

  if (e.currentTarget && e.currentTarget instanceof Element) {
    const rect = e.currentTarget.getBoundingClientRect();
    const offsetX = e.clientX - rect.left;
    const offsetY = e.clientY - rect.top;
    return new Vector2D({x: offsetX, y: offsetY});
  }

  return new Vector2D({x: e.offsetX, y: e.offsetY});
}

// Adds an event listener to a DOM element, returning a disposable to remove it.
export function bindEventListener<K extends keyof HTMLElementEventMap>(
  element: EventTarget,
  event: K,
  handler: (event: HTMLElementEventMap[K]) => void,
  options?: AddEventListenerOptions,
): Disposable {
  element.addEventListener(event, handler as EventListener, options);
  return {
    [Symbol.dispose]() {
      element.removeEventListener(event, handler as EventListener);
    },
  };
}

export interface DragEvent {
  // Movement delta since the previous event.
  delta: {readonly x: number; readonly y: number};
  // Absolute pointer position in client coordinates.
  client: {readonly x: number; readonly y: number};
  // Pointer position at the very start of the drag in client coordinates.
  startClient: {readonly x: number; readonly y: number};
}

// Waits for a drag gesture to begin (pointer moved beyond `deadzone` px).
// Returns an async iterable of DragEvents, or undefined if the pointer was
// released before the deadzone was crossed (i.e. it was a click). The first yielded
// event includes accumulated movement from the deadzone phase so callers never
// lose movement that occurred before the drag was confirmed.
export async function captureDrag(attrs: {
  el: HTMLElement;
  e: PointerEvent;
  deadzone?: number;
}): Promise<AsyncIterable<DragEvent> | undefined> {
  const {el, e, deadzone = 0} = attrs;
  const pointerId = e.pointerId;

  el.setPointerCapture(pointerId);

  let resolveNext: ((e: PointerEvent | undefined) => void) | undefined;

  const onMove = (e: PointerEvent) => {
    if (e.pointerId !== pointerId) return;
    resolveNext?.(e);
    resolveNext = undefined;
  };
  const onDone = (e: PointerEvent) => {
    if (e.pointerId !== pointerId) return;
    resolveNext?.(undefined);
    resolveNext = undefined;
  };

  el.addEventListener('pointermove', onMove);
  el.addEventListener('pointerup', onDone);
  el.addEventListener('pointercancel', onDone);

  const cleanup = () => {
    el.removeEventListener('pointermove', onMove);
    el.removeEventListener('pointerup', onDone);
    el.removeEventListener('pointercancel', onDone);
  };

  const next = () =>
    new Promise<PointerEvent | undefined>((r) => {
      resolveNext = r;
    });

  // Phase 1: wait for deadzone to be crossed, or pointerup (→ click).
  // Accumulate movement so the first yield includes the full delta.
  let accum = new Vector2D({x: 0, y: 0});
  const start = new Vector2D({x: e.clientX, y: e.clientY});
  let firstEvent: PointerEvent = e;
  while (deadzone > 0) {
    const ev = await next();
    if (ev === undefined) {
      cleanup();
      return undefined;
    }
    firstEvent = ev;
    accum = accum.add({x: ev.movementX, y: ev.movementY});
    if (start.sub({x: ev.clientX, y: ev.clientY}).magnitude >= deadzone) break;
  }

  const startClient = {x: e.clientX, y: e.clientY};

  // Phase 2: drag confirmed — stream events to the caller, leading with the
  // accumulated deadzone movement as the first yield.
  return (async function* () {
    try {
      if (deadzone > 0) {
        yield {
          delta: accum,
          client: {x: firstEvent.clientX, y: firstEvent.clientY},
          startClient,
        };
      }
      while (true) {
        const ev = await next();
        if (ev === undefined) return;
        yield {
          delta: {x: ev.movementX, y: ev.movementY},
          client: {x: ev.clientX, y: ev.clientY},
          startClient,
        };
      }
    } finally {
      cleanup();
    }
  })();
}
