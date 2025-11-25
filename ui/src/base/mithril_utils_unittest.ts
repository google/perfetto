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
import {createContext} from './mithril_utils';

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
