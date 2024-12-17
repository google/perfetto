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

import {ZonedInteractionHandler} from './zoned_interaction_handler';

describe('ZonedInteractionHandler', () => {
  let zih: ZonedInteractionHandler;
  let div: HTMLElement;

  beforeEach(() => {
    // Create a DOM element
    div = document.createElement('div');
    div.style.width = '100px';
    div.style.height = '100px';
    document.body.appendChild(div);
    zih = new ZonedInteractionHandler(div);
  });

  // Utility functions for simulating mouse events
  function mouseup(x: number, y: number) {
    simulateMouseEvent('mouseup', x, y);
  }

  function mousedown(x: number, y: number) {
    simulateMouseEvent('mousedown', x, y);
  }

  function mousemove(x: number, y: number) {
    simulateMouseEvent('mousemove', x, y);
  }

  function simulateMouseEvent(kind: string, x: number, y: number) {
    div.dispatchEvent(
      new MouseEvent(kind, {
        bubbles: true,
        clientX: x,
        clientY: y,
      }),
    );
  }

  test('overlapping zones', () => {
    zih.update([
      {
        id: 'foo',
        area: {x: 50, y: 50, width: 50, height: 50},
        cursor: 'grab',
      },
      {
        id: 'bar',
        area: {x: 0, y: 0, width: 100, height: 100},
        cursor: 'pointer',
      },
    ]);

    mousemove(30, 30); // inside 'bar'
    expect(div.style.cursor).toBe('pointer');

    mousemove(70, 70); // inside 'foo'
    expect(div.style.cursor).toBe('grab');
  });

  test('click', () => {
    const handleMouseClick = jest.fn(() => {});

    zih.update([
      {
        id: 'foo',
        area: {x: 0, y: 0, width: 60, height: 60},
        onClick: handleMouseClick,
      },
    ]);

    // Simulate a mouse click
    mousedown(50, 50);
    mouseup(50, 50);

    expect(handleMouseClick).toHaveBeenCalled();

    handleMouseClick.mockClear();

    // Simulate a mouse down then a mouseup outside the zone
    mousedown(50, 50);
    mouseup(80, 80);

    expect(handleMouseClick).not.toHaveBeenCalled();
  });

  test('drag', () => {
    const handleDrag = jest.fn(() => {});
    const handleDragEnd = jest.fn(() => {});

    zih.update([
      {
        id: 'foo',
        area: {x: 0, y: 0, width: 100, height: 100},
        drag: {
          cursorWhileDragging: 'grabbing',
          onDrag: handleDrag,
          onDragEnd: handleDragEnd,
        },
      },
    ]);

    // Simulate a mouse drag start
    mousedown(0, 0);
    expect(div.style.cursor).toBe('grabbing');

    // Simulate a mouse drag move
    mousemove(50, 0);

    expect(handleDrag).toHaveBeenCalled();

    // Simulate a drag end
    mouseup(60, 0);
    expect(handleDragEnd).toHaveBeenCalled();
  });

  test('drag with minimum distance', () => {
    const handleDrag = jest.fn();
    const handleDragEnd = jest.fn();

    zih.update([
      {
        id: 'dragZone',
        area: {x: 0, y: 0, width: 100, height: 100},
        drag: {
          minDistance: 20,
          onDrag: handleDrag,
          onDragEnd: handleDragEnd,
        },
      },
    ]);

    // Simulate drag start
    mousedown(10, 10);

    // Move within the minimum distance
    mousemove(15, 15);
    expect(handleDrag).not.toHaveBeenCalled();

    // Move beyond the minimum distance
    mousemove(40, 40);
    expect(handleDrag).toHaveBeenCalled();

    // End the drag
    mouseup(50, 50);
    expect(handleDragEnd).toHaveBeenCalled();
  });

  test('onWheel', () => {
    const handleWheel = jest.fn();

    zih.update([
      {
        id: 'foo',
        area: {x: 0, y: 0, width: 100, height: 100},
        onWheel: handleWheel,
      },
    ]);

    // Simulate a wheel event inside the zone
    div.dispatchEvent(
      new WheelEvent('wheel', {
        bubbles: true,
        clientX: 50,
        clientY: 50,
        deltaX: 5,
        deltaY: 10,
      }),
    );

    expect(handleWheel).toHaveBeenCalled();
    expect(handleWheel.mock.calls[0][0]).toMatchObject({
      position: {x: 50, y: 50},
      deltaX: 5,
      deltaY: 10,
    });
  });

  test('key modifiers', () => {
    const handleMouseClick = jest.fn();

    zih.update([
      {
        id: 'modifierZone',
        area: {x: 0, y: 0, width: 100, height: 100},
        keyModifier: 'shift',
        onClick: handleMouseClick,
      },
    ]);

    // Attempt click without holding the modifier key
    mousedown(50, 50);
    mouseup(50, 50);
    expect(handleMouseClick).not.toHaveBeenCalled();

    // Simulate holding down the shift key and clicking
    document.dispatchEvent(new KeyboardEvent('keydown', {shiftKey: true}));
    mousedown(50, 50);
    mouseup(50, 50);
    expect(handleMouseClick).toHaveBeenCalled();

    // Simulate releasing the shift key
    document.dispatchEvent(new KeyboardEvent('keyup', {shiftKey: false}));
    mousedown(50, 50);
    mouseup(50, 50);
    expect(handleMouseClick).toHaveBeenCalledTimes(1); // No additional call
  });

  test('move zone during drag', () => {
    const handleDrag = jest.fn();
    const handleDragEnd = jest.fn();

    zih.update([
      {
        id: 'dragZone',
        area: {x: 0, y: 0, width: 100, height: 100},
        drag: {
          onDrag: handleDrag,
          onDragEnd: handleDragEnd,
        },
      },
    ]);

    // Start a drag
    mousedown(10, 10);

    // Update zones while dragging
    zih.update([
      {
        id: 'dragZone',
        area: {x: 0, y: 0, width: 10, height: 10},
        drag: {
          onDrag: handleDrag,
          onDragEnd: handleDragEnd,
        },
      },
    ]);

    // Continue dragging - drags are sticky, so even if we drag outside of the
    // zone, the drag persists
    mousemove(50, 50);
    expect(handleDrag).toHaveBeenCalled();

    // End drag
    mouseup(60, 60);
    expect(handleDragEnd).toHaveBeenCalled();
  });

  test('click and move but stay in zone', () => {
    const handleMouseClick = jest.fn(() => {});

    zih.update([
      {
        id: 'foo',
        area: {x: 0, y: 0, width: 60, height: 60},
        onClick: handleMouseClick,
      },
    ]);

    // Simulate a mouse click where the cursor has moved a little by remains
    // inside the zone with the click event handler.
    mousedown(30, 30);
    mouseup(50, 50);

    expect(handleMouseClick).toHaveBeenCalled();
  });

  test('click and move out of zone', () => {
    const handleMouseClick = jest.fn(() => {});

    zih.update([
      {
        id: 'foo',
        area: {x: 0, y: 0, width: 60, height: 60},
        onClick: handleMouseClick,
      },
    ]);

    // Simulate a mouse click where the cursor has moved outside of the zone.
    mousedown(50, 50);
    mouseup(80, 80);

    expect(handleMouseClick).not.toHaveBeenCalled();
  });

  afterEach(() => {
    document.body.removeChild(div);
  });
});
