// Copyright (C) 2024 The Android Open Source Project
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

/**
 * This module provides an extensible, declarative interaction manager for
 * handling high level mouse and keyboard interactions within an HTML element,
 * using zones to define areas with different configurations.
 *
 * This is typically used on canvas, where we want to create draggable handles,
 * or area selections, but there are no fine-grained DOM elements we can attach
 * to.
 *
 * It supports:
 * - Specifying a list of zones, which can specify their own mouse event
 *   handlers.
 * - Changing the cursor when hovering over a zone.
 * - High level drag event handlers with customizable drag thresholds, 'while
 *   dragging' cursors and keyboard modifiers.
 * - Click event handlers, which integrate nicely with drag events (i.e. failed
 *   drag events turn into clicks).
 * - Mouse wheel events.
 *
 * How it works:
 *
 * For events that fire on the given target element, the list of zones is
 * searched from top to bottom until a zone that handles that event is found.
 *
 * The list of zones is declarative, and is designed to be updated frequently
 * i.e. every frame. This means that long running events such as drags can be
 * can outlive the a single update cycle. Each zone must specify an id which is
 * a unique string used to link up the new zones with ongoing drag events, and
 * thus use the new callbacks. This is important as new callbacks might capture
 * different data.
 */

import {removeFalsyValues} from './array_utils';
import {DisposableStack} from './disposable_stack';
import {bindEventListener, CSSCursor} from './dom_utils';
import {Point2D, Rect2D, Size2D, Vector2D} from './geom';
import {convertTouchIntoMouseEvents} from './touchscreen_handler';

export interface DragEvent {
  // The location of the mouse at the start of the drag action.
  readonly dragStart: Vector2D;

  // The location of the mouse currently.
  readonly dragCurrent: Vector2D;

  // The amount the mouse has moved by duration the drag.
  // I.e. currentMousePosition - startingMousePosition
  readonly dragDelta: Vector2D;

  // The amount the mouse have moved by since the last drag event.
  readonly deltaSinceLastEvent: Vector2D;

  // Whether the Alt key is held.
  readonly altKey: boolean;

  // Whether the Shift key is held.
  readonly shiftKey: boolean;

  // Whether the Ctrl key is held.
  readonly ctrlKey: boolean;

  // Whether the Meta key is held.
  readonly metaKey: boolean;
}

export interface ClickEvent {
  // Location of the mouse W.R.T the target element (not the current zone).
  readonly position: Vector2D;
}

export interface InteractionWheelEvent {
  // Location of the mouse W.R.T the target element (not the current zone).
  readonly position: Vector2D;

  // Wheel deltaX directly from the DOM WheelEvent.
  readonly deltaX: number;

  // Wheel deltaY directly from the DOM WheelEvent.
  readonly deltaY: number;

  // Whether the ctrl key is held or not.
  readonly ctrlKey: boolean;
}

export interface DragConfig {
  // Optional: Switch to this cursor while dragging.
  readonly cursorWhileDragging?: CSSCursor;

  // The minimum distance the mouse must move before a drag is triggered.
  // Default: 0 - drags start instantly.
  readonly minDistance?: number;

  onDragStart?(e: DragEvent, element: HTMLElement): void;

  // Optional: Called whenever the mouse is moved during a drag event.
  onDrag?(e: DragEvent, element: HTMLElement): void;

  // Optional: Called when the mouse button is released and the drag is complete.
  onDragEnd?(e: DragEvent, element: HTMLElement): void;
}

export interface Zone {
  // Unique ID for this zone. This is used to coordinate long events such as
  // drag event callbacks between update cycles.
  readonly id: string;

  // The area occupied by this zone.
  readonly area: Point2D & Size2D;

  // Optional: Which cursor to change the mouse to when hovering over this zone.
  readonly cursor?: CSSCursor;

  // Optional: If present, this keyboard modifier must be held otherwise this
  // zone is effectively invisible to interactions.
  readonly keyModifier?: 'shift';

  // Optional: If present, this zone will respond to drag events.
  readonly drag?: DragConfig;

  // Optional: If present, this function will be called when this zone is
  // clicked on.
  onClick?(e: ClickEvent): void;

  // Optional: If present, this function will be called when the wheel is
  // scrolled while hovering over this zone.
  onWheel?(e: InteractionWheelEvent): void;
}

interface InProgressGesture {
  readonly zoneId: string;
  readonly startingMousePosition: Vector2D;
  currentMousePosition: Vector2D;
  previouslyNotifiedPosition: Vector2D;
}

export class ZonedInteractionHandler implements Disposable {
  private readonly trash = new DisposableStack();
  private currentMousePosition?: Point2D;
  private zones: ReadonlyArray<Zone> = [];
  private currentGesture?: InProgressGesture;
  private shiftHeld = false;

  constructor(readonly target: HTMLElement) {
    this.bindEvent(this.target, 'mousedown', this.onMouseDown.bind(this));
    this.bindEvent(document, 'mousemove', this.onMouseMove.bind(this));
    this.bindEvent(document, 'mouseup', this.onMouseUp.bind(this));
    this.bindEvent(document, 'keydown', this.onKeyDown.bind(this));
    this.bindEvent(document, 'keyup', this.onKeyUp.bind(this));
    this.bindEvent(this.target, 'wheel', this.handleWheel.bind(this));
    this.trash.use(
      convertTouchIntoMouseEvents(this.target, [
        'down-up-move',
        'pan-x',
        'pinch-zoom-as-ctrl-wheel',
      ]),
    );
  }

  [Symbol.dispose](): void {
    this.trash.dispose();
  }

  /**
   * Update the list of zones and their configurations. Each zone is processed
   * from the start to the end of the list, so zones which appear earlier in the
   * list will be chosen before those later in the list.
   *
   * Zones can be falsy, which allows the simple conditional zones to be defined
   * using short circuits, similar to mithril. Falsy zones are simply ignored.
   *
   * @param zones - The list of zones to configure interactions areas and their
   * configurations.
   */
  update(zones: ReadonlyArray<Zone | false | undefined | null>): void {
    this.zones = removeFalsyValues(zones);
    this.updateCursor();
  }

  // Utility function to bind an event listener to a DOM element and add it to
  // the trash.
  private bindEvent<K extends keyof HTMLElementEventMap>(
    element: EventTarget,
    event: K,
    handler: (event: HTMLElementEventMap[K]) => void,
  ) {
    this.trash.use(bindEventListener(element, event, handler));
  }

  private onMouseDown(e: MouseEvent) {
    const mousePositionClient = new Vector2D({x: e.clientX, y: e.clientY});
    const mouse = mousePositionClient.sub(this.target.getBoundingClientRect());
    const zone = this.findZone(
      (z) => (z.drag || z.onClick) && this.hitTestZone(z, mouse),
    );
    if (zone) {
      this.currentGesture = {
        zoneId: zone.id,
        startingMousePosition: mouse,
        currentMousePosition: mouse,
        previouslyNotifiedPosition: mouse,
      };
      this.updateCursor();
    }
  }

  private onMouseMove(e: MouseEvent) {
    const mousePositionClient = new Vector2D({x: e.clientX, y: e.clientY});
    const mousePosition = mousePositionClient.sub(
      this.target.getBoundingClientRect(),
    );
    this.currentMousePosition = mousePosition;
    this.updateCursor();

    const currentDrag = this.currentGesture;
    if (currentDrag) {
      currentDrag.currentMousePosition = mousePosition;
      const delta = currentDrag.startingMousePosition.sub(mousePosition);
      const dragConfig = this.findZoneById(currentDrag.zoneId)?.drag;
      if (
        dragConfig &&
        delta.manhattanDistance >= (dragConfig?.minDistance ?? 0)
      ) {
        dragConfig.onDrag?.(
          {
            dragCurrent: mousePosition,
            dragStart: currentDrag.startingMousePosition,
            dragDelta: delta,
            deltaSinceLastEvent: mousePosition.sub(
              currentDrag.previouslyNotifiedPosition,
            ),
            altKey: e.altKey,
            shiftKey: e.shiftKey,
            ctrlKey: e.ctrlKey,
            metaKey: e.metaKey,
          },
          this.target,
        );
        currentDrag.previouslyNotifiedPosition = mousePosition;
      }
    }
  }

  private onMouseUp(e: MouseEvent) {
    const mousePositionClient = new Vector2D({x: e.clientX, y: e.clientY});
    const mouse = mousePositionClient.sub(this.target.getBoundingClientRect());

    const gesture = this.currentGesture;

    if (gesture) {
      const delta = gesture.startingMousePosition.sub(mouse);
      const zone = this.findZoneById(gesture.zoneId);
      if (zone) {
        if (
          zone.drag &&
          delta.manhattanDistance >= (zone.drag?.minDistance ?? 0)
        ) {
          this.handleDrag(this.target, gesture, mouse, e, zone.drag);
        } else {
          // Check we're still the zone the click was started in
          if (this.hitTestZone(zone, mouse)) {
            this.handleClick(this.target, e);
          }
        }
      }

      this.currentGesture = undefined;
      this.updateCursor();
    }
  }

  private onKeyDown(e: KeyboardEvent) {
    this.shiftHeld = e.shiftKey;
    this.updateCursor();
  }

  private onKeyUp(e: KeyboardEvent) {
    this.shiftHeld = e.shiftKey;
    this.updateCursor();
  }

  private handleWheel(e: WheelEvent) {
    const mousePositionClient = new Vector2D({x: e.clientX, y: e.clientY});
    const mouse = mousePositionClient.sub(this.target.getBoundingClientRect());
    const zone = this.findZone((z) => z.onWheel && this.hitTestZone(z, mouse));
    zone?.onWheel?.({
      position: mouse,
      deltaX: e.deltaX,
      deltaY: e.deltaY,
      ctrlKey: e.ctrlKey,
    });
  }

  private handleDrag(
    element: HTMLElement,
    currentDrag: InProgressGesture,
    x: Vector2D,
    e: MouseEvent,
    dragConfig: DragConfig,
  ) {
    // Update the current position
    currentDrag.currentMousePosition = x;

    const dragEvent: DragEvent = {
      dragStart: currentDrag.startingMousePosition,
      dragCurrent: x,
      dragDelta: new Vector2D({x: e.movementX, y: e.movementY}),
      deltaSinceLastEvent: new Vector2D({x: e.movementX, y: e.movementY}),
      altKey: e.altKey,
      shiftKey: e.shiftKey,
      ctrlKey: e.ctrlKey,
      metaKey: e.metaKey,
    };

    dragConfig.onDragEnd?.(dragEvent, element);
  }

  private handleClick(element: HTMLElement, e: MouseEvent) {
    const mousePositionClient = new Vector2D({x: e.clientX, y: e.clientY});
    const mouse = mousePositionClient.sub(element.getBoundingClientRect());
    const zone = this.findZone((z) => z.onClick && this.hitTestZone(z, mouse));
    zone?.onClick?.({position: mouse});
  }

  private updateCursor() {
    // If a drag is ongoing, use the drag cursor if available
    const drag = this.currentGesture;
    if (drag) {
      const dragDelta = drag.currentMousePosition.sub(
        drag.startingMousePosition,
      );
      const dragConfig = this.findZoneById(drag.zoneId)?.drag;
      if (
        dragConfig &&
        dragConfig.cursorWhileDragging &&
        dragDelta.manhattanDistance >= (dragConfig.minDistance ?? 0)
      ) {
        this.target.style.cursor = dragConfig.cursorWhileDragging;
        return;
      }
    }

    // Otherwise, find the hovered zone and set the cursor
    const mouse = this.currentMousePosition;
    const zone =
      mouse && this.findZone((z) => z.cursor && this.hitTestZone(z, mouse));
    this.target.style.cursor = zone?.cursor ?? 'default';
  }

  // Find a zone that matches a predicate.
  private findZone(pred: (z: Zone) => boolean | undefined): Zone | undefined {
    for (const zone of this.zones) {
      if (pred(zone)) return zone;
    }
    return undefined;
  }

  // Find a zone by id.
  private findZoneById(id: string): Zone | undefined {
    for (const zone of this.zones) {
      if (zone.id === id) return zone;
    }
    return undefined;
  }

  // Test whether a point hits a zone.
  private hitTestZone(zone: Zone, x: Point2D): boolean {
    const rect = Rect2D.fromPointAndSize(zone.area);
    return rect.containsPoint(x) && (!zone.keyModifier || this.shiftHeld);
  }
}
