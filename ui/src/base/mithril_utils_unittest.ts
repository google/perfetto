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

import m from 'mithril';
import {createContext, startDragGesture} from './mithril_utils';

// jsdom doesn't implement PointerEvent. Build a minimal stand-in by
// extending MouseEvent with a pointerId — sufficient for startDragGesture,
// which only reads clientX/clientY and pointerId.
function makePointerEvent(
  type: string,
  init: {clientX?: number; clientY?: number; pointerId?: number} = {},
): PointerEvent {
  const ev = new MouseEvent(type, {
    bubbles: true,
    cancelable: true,
    clientX: init.clientX ?? 0,
    clientY: init.clientY ?? 0,
  }) as MouseEvent & {pointerId: number};
  ev.pointerId = init.pointerId ?? 1;
  return ev as unknown as PointerEvent;
}

function makeTarget(): HTMLElement {
  const el = document.createElement('div');
  document.body.appendChild(el);
  // jsdom lacks pointer-capture; stub the methods startDragGesture calls.
  el.setPointerCapture = () => {};
  el.releasePointerCapture = () => {};
  el.hasPointerCapture = () => false;
  return el;
}

describe('startDragGesture', () => {
  afterEach(() => {
    document.body.innerHTML = '';
  });

  test('invokes onDragFailed when released inside deadzone', () => {
    const el = makeTarget();
    const onDrag = jest.fn();
    const onDragEnd = jest.fn();
    const onDragFailed = jest.fn();

    const down = makePointerEvent('pointerdown', {clientX: 100, clientY: 100});
    Object.defineProperty(down, 'currentTarget', {value: el});
    startDragGesture({e: down, deadzonePx: 5, onDrag, onDragEnd, onDragFailed});

    el.dispatchEvent(
      makePointerEvent('pointerup', {clientX: 101, clientY: 101}),
    );

    expect(onDrag).not.toHaveBeenCalled();
    expect(onDragEnd).not.toHaveBeenCalled();
    expect(onDragFailed).toHaveBeenCalledTimes(1);
  });

  test('top-level onDrag/onDragEnd fire after deadzone is crossed', () => {
    const el = makeTarget();
    const onDrag = jest.fn();
    const onDragEnd = jest.fn();
    const onDragFailed = jest.fn();
    const onDragStart = jest.fn();

    const down = makePointerEvent('pointerdown', {clientX: 0, clientY: 0});
    Object.defineProperty(down, 'currentTarget', {value: el});
    startDragGesture({
      e: down,
      deadzonePx: 5,
      onDragStart,
      onDrag,
      onDragEnd,
      onDragFailed,
    });

    // Inside deadzone — nothing fires.
    el.dispatchEvent(makePointerEvent('pointermove', {clientX: 2, clientY: 2}));
    expect(onDragStart).not.toHaveBeenCalled();
    expect(onDrag).not.toHaveBeenCalled();

    // Crosses deadzone — onDragStart fires, but this move is consumed by it.
    el.dispatchEvent(
      makePointerEvent('pointermove', {clientX: 10, clientY: 0}),
    );
    expect(onDragStart).toHaveBeenCalledTimes(1);
    expect(onDrag).not.toHaveBeenCalled();

    // Subsequent moves go to the top-level onDrag.
    el.dispatchEvent(
      makePointerEvent('pointermove', {clientX: 20, clientY: 0}),
    );
    el.dispatchEvent(
      makePointerEvent('pointermove', {clientX: 30, clientY: 0}),
    );
    expect(onDrag).toHaveBeenCalledTimes(2);

    el.dispatchEvent(makePointerEvent('pointerup', {clientX: 30, clientY: 0}));
    expect(onDragEnd).toHaveBeenCalledTimes(1);
    expect(onDragFailed).not.toHaveBeenCalled();
  });

  test('works with no onDragStart at all (direct mode)', () => {
    const el = makeTarget();
    const onDrag = jest.fn();
    const onDragEnd = jest.fn();

    const down = makePointerEvent('pointerdown');
    Object.defineProperty(down, 'currentTarget', {value: el});
    startDragGesture({e: down, onDrag, onDragEnd});

    el.dispatchEvent(
      makePointerEvent('pointermove', {clientX: 50, clientY: 0}),
    );
    el.dispatchEvent(
      makePointerEvent('pointermove', {clientX: 60, clientY: 0}),
    );
    el.dispatchEvent(makePointerEvent('pointerup', {clientX: 60, clientY: 0}));

    expect(onDrag).toHaveBeenCalledTimes(2);
    expect(onDragEnd).toHaveBeenCalledTimes(1);
  });

  test('per-gesture handlers from onDragStart override top-level ones', () => {
    const el = makeTarget();
    const topOnDrag = jest.fn();
    const topOnDragEnd = jest.fn();
    const perOnDrag = jest.fn();
    const perOnDragEnd = jest.fn();

    const down = makePointerEvent('pointerdown');
    Object.defineProperty(down, 'currentTarget', {value: el});
    startDragGesture({
      e: down,
      onDragStart: () => ({onDrag: perOnDrag, onDragEnd: perOnDragEnd}),
      onDrag: topOnDrag,
      onDragEnd: topOnDragEnd,
    });

    el.dispatchEvent(
      makePointerEvent('pointermove', {clientX: 50, clientY: 0}),
    );
    el.dispatchEvent(
      makePointerEvent('pointermove', {clientX: 60, clientY: 0}),
    );
    el.dispatchEvent(makePointerEvent('pointerup', {clientX: 60, clientY: 0}));

    expect(perOnDrag).toHaveBeenCalledTimes(2);
    expect(perOnDragEnd).toHaveBeenCalledTimes(1);
    expect(topOnDrag).not.toHaveBeenCalled();
    expect(topOnDragEnd).not.toHaveBeenCalled();
  });

  test('listeners are removed after gesture ends', () => {
    const el = makeTarget();
    const onDrag = jest.fn();
    const onDragEnd = jest.fn();

    const down = makePointerEvent('pointerdown');
    Object.defineProperty(down, 'currentTarget', {value: el});
    startDragGesture({e: down, onDrag, onDragEnd});

    el.dispatchEvent(
      makePointerEvent('pointermove', {clientX: 50, clientY: 0}),
    );
    el.dispatchEvent(makePointerEvent('pointerup', {clientX: 50, clientY: 0}));

    onDrag.mockClear();
    onDragEnd.mockClear();
    el.dispatchEvent(
      makePointerEvent('pointermove', {clientX: 100, clientY: 0}),
    );
    el.dispatchEvent(makePointerEvent('pointerup', {clientX: 100, clientY: 0}));

    expect(onDrag).not.toHaveBeenCalled();
    expect(onDragEnd).not.toHaveBeenCalled();
  });

  test('deadzonePx: 0 starts the drag immediately on pointerdown', () => {
    const el = makeTarget();
    const onDragStart = jest.fn();
    const onDrag = jest.fn();
    const onDragEnd = jest.fn();
    const onDragFailed = jest.fn();

    const down = makePointerEvent('pointerdown', {clientX: 10, clientY: 10});
    Object.defineProperty(down, 'currentTarget', {value: el});
    startDragGesture({
      e: down,
      deadzonePx: 0,
      onDragStart,
      onDrag,
      onDragEnd,
      onDragFailed,
    });

    // onDragStart fires synchronously, before any pointermove.
    expect(onDragStart).toHaveBeenCalledTimes(1);

    // A release without moving counts as drag end, not drag failed.
    el.dispatchEvent(makePointerEvent('pointerup', {clientX: 10, clientY: 10}));
    expect(onDragEnd).toHaveBeenCalledTimes(1);
    expect(onDragFailed).not.toHaveBeenCalled();
  });

  test('pointercancel ends the gesture like pointerup', () => {
    const el = makeTarget();
    const onDragEnd = jest.fn();
    const onDragFailed = jest.fn();

    const down = makePointerEvent('pointerdown');
    Object.defineProperty(down, 'currentTarget', {value: el});
    startDragGesture({e: down, onDragEnd, onDragFailed});

    el.dispatchEvent(
      makePointerEvent('pointermove', {clientX: 50, clientY: 0}),
    );
    el.dispatchEvent(
      makePointerEvent('pointercancel', {clientX: 50, clientY: 0}),
    );

    expect(onDragEnd).toHaveBeenCalledTimes(1);
    expect(onDragFailed).not.toHaveBeenCalled();
  });
});

describe('createContext', () => {
  test('provides default value to consumers', () => {
    const {Consumer} = createContext('default');

    let receivedValue: string | undefined;
    const TestComponent = {
      view: () =>
        m(Consumer, (value) => {
          receivedValue = value;
        }),
    };

    m.render(document.body, m(TestComponent));
    expect(receivedValue).toBe('default');
  });

  test('provides undefined when no default value', () => {
    const {Consumer} = createContext<string>();

    let receivedValue: string | undefined = 'sentinel';
    const TestComponent = {
      view: () =>
        m(Consumer, (value) => {
          receivedValue = value;
        }),
    };

    m.render(document.body, m(TestComponent));
    expect(receivedValue).toBeUndefined();
  });

  test('provider overrides default value', () => {
    const {Provider, Consumer} = createContext('default');

    let receivedValue: string | undefined;
    const TestComponent = {
      view: () =>
        m(
          Provider,
          {value: 'custom'},
          m(Consumer, (value) => {
            receivedValue = value;
          }),
        ),
    };

    m.render(document.body, m(TestComponent));
    expect(receivedValue).toBe('custom');
  });

  test('nested providers use innermost value', () => {
    const {Provider, Consumer} = createContext('default');

    const receivedValues: string[] = [];
    const TestComponent = {
      view: () =>
        m(Provider, {value: 'outer'}, [
          m(Consumer, (value) => {
            receivedValues.push(value);
          }),
          m(
            Provider,
            {value: 'inner'},
            m(Consumer, (value) => {
              receivedValues.push(value);
            }),
          ),
          m(Consumer, (value) => {
            receivedValues.push(value);
          }),
        ]),
    };

    m.render(document.body, m(TestComponent));
    expect(receivedValues).toEqual(['outer', 'inner', 'outer']);
  });

  test('multiple providers in parallel', () => {
    const {Provider, Consumer} = createContext('default');

    const receivedValues: string[] = [];
    const TestComponent = {
      view: () => [
        m(Consumer, (value) => {
          receivedValues.push(value);
        }),
        m(
          Provider,
          {value: 'foo'},
          m(Consumer, (value) => {
            receivedValues.push(value);
          }),
        ),
        m(
          Provider,
          {value: 'bar'},
          m(Consumer, (value) => {
            receivedValues.push(value);
          }),
        ),
        m(Consumer, (value) => {
          receivedValues.push(value);
        }),
      ],
    };

    m.render(document.body, m(TestComponent));
    expect(receivedValues).toEqual(['default', 'foo', 'bar', 'default']);
  });

  test('different contexts are independent', () => {
    const Context1 = createContext('default1');
    const Context2 = createContext('default2');

    const receivedValues: string[] = [];
    const TestComponent = {
      view: () =>
        m(Context1.Provider, {value: 'value1'}, [
          m(Context2.Provider, {value: 'value2'}, [
            m(Context1.Consumer, (value) => {
              receivedValues.push(value);
            }),
            m(Context2.Consumer, (value) => {
              receivedValues.push(value);
            }),
          ]),
        ]),
    };

    m.render(document.body, m(TestComponent));
    expect(receivedValues).toEqual(['value1', 'value2']);
  });

  test('works with complex types', () => {
    interface User {
      name: string;
      age: number;
    }

    const {Provider, Consumer} = createContext<User>({name: 'Default', age: 0});

    let receivedValue: User | undefined;
    const TestComponent = {
      view: () =>
        m(
          Provider,
          {value: {name: 'Alice', age: 30}},
          m(Consumer, (value) => {
            receivedValue = value;
          }),
        ),
    };

    m.render(document.body, m(TestComponent));
    expect(receivedValue).toEqual({name: 'Alice', age: 30});
  });

  test('handles null values', () => {
    const {Provider, Consumer} = createContext<string | null>('default');

    let receivedValue: string | null = 'sentinel';
    const TestComponent = {
      view: () =>
        m(
          Provider,
          {value: null},
          m(Consumer, (value) => {
            receivedValue = value;
          }),
        ),
    };

    m.render(document.body, m(TestComponent));
    expect(receivedValue).toBeNull();
  });
});
