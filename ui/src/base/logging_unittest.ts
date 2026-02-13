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

import {
  assertDefined,
  assertExists,
  assertFalse,
  assertIsInstanceOf,
  assertTrue,
  assertUnreachable,
  fail,
} from './logging';

describe('assertExists', () => {
  test('returns value when not null or undefined', () => {
    expect(assertExists(42)).toBe(42);
    expect(assertExists('hello')).toBe('hello');
    expect(assertExists(0)).toBe(0);
    expect(assertExists('')).toBe('');
    expect(assertExists(false)).toBe(false);
  });

  test('throws on null', () => {
    expect(() => assertExists(null)).toThrow("`<expression>` doesn't exist");
  });

  test('throws on undefined', () => {
    expect(() => assertExists(undefined)).toThrow(
      "`<expression>` doesn't exist",
    );
  });

  test('includes description in error message', () => {
    expect(() => assertExists(null, 'foo.bar')).toThrow(
      "`foo.bar` doesn't exist",
    );
  });
});

describe('assertDefined', () => {
  test('returns value when defined', () => {
    expect(assertDefined(42)).toBe(42);
    expect(assertDefined(null)).toBe(null); // null is defined
    expect(assertDefined(0)).toBe(0);
    expect(assertDefined('')).toBe('');
  });

  test('throws on undefined', () => {
    expect(() => assertDefined(undefined)).toThrow(
      '`<expression>` is undefined',
    );
  });

  test('includes description in error message', () => {
    expect(() => assertDefined(undefined, 'config.option')).toThrow(
      '`config.option` is undefined',
    );
  });
});

describe('assertTrue', () => {
  test('does not throw when value is true', () => {
    expect(() => assertTrue(true)).not.toThrow();
  });

  test('throws when value is false', () => {
    expect(() => assertTrue(false)).toThrow('`<expression>` is falsy');
  });

  test('includes description in error message', () => {
    expect(() => assertTrue(false, 'x > 0')).toThrow('`x > 0` is falsy');
  });
});

describe('assertFalse', () => {
  test('does not throw when value is false', () => {
    expect(() => assertFalse(false)).not.toThrow();
  });

  test('throws when value is true', () => {
    expect(() => assertFalse(true)).toThrow('`<expression>` is truthy');
  });

  test('includes description in error message', () => {
    expect(() => assertFalse(true, 'isDisabled')).toThrow(
      '`isDisabled` is truthy',
    );
  });
});

describe('assertIsInstanceOf', () => {
  test('returns value when instance matches', () => {
    const arr = [1, 2, 3];
    expect(assertIsInstanceOf(arr, Array)).toBe(arr);

    const date = new Date();
    expect(assertIsInstanceOf(date, Date)).toBe(date);
  });

  test('throws when instance does not match', () => {
    expect(() => assertIsInstanceOf({}, Array)).toThrow(
      '`<expression>` is not an instance of Array',
    );
  });

  test('includes description in error message', () => {
    expect(() => assertIsInstanceOf('hello', Array, 'myVar')).toThrow(
      '`myVar` is not an instance of Array',
    );
  });
});

describe('fail', () => {
  test('always throws with the given message', () => {
    expect(() => fail('something went wrong')).toThrow('something went wrong');
  });
});

describe('assertUnreachable', () => {
  test('throws with value in error message', () => {
    // We need to cast to never to test this
    const value = 'unexpected' as never;
    expect(() => assertUnreachable(value)).toThrow(
      'Unreachable code reached when `<expression>` = "unexpected"',
    );
  });

  test('includes description in error message', () => {
    const value = 'oops' as never;
    expect(() => assertUnreachable(value, 'mode')).toThrow(
      'Unreachable code reached when `mode` = "oops"',
    );
  });

  test('handles numeric values', () => {
    const value = 42 as never;
    expect(() => assertUnreachable(value, 'count')).toThrow(
      'Unreachable code reached when `count` = 42',
    );
  });

  test('handles object values', () => {
    const value = {foo: 'bar'} as never;
    expect(() => assertUnreachable(value, 'obj')).toThrow(
      'Unreachable code reached when `obj` = {"foo":"bar"}',
    );
  });

  test('handles bigint values', () => {
    const value = BigInt(123) as never;
    expect(() => assertUnreachable(value, 'big')).toThrow(
      'Unreachable code reached when `big` = "123n"',
    );
  });

  test('handles symbol values', () => {
    const value = Symbol('test') as never;
    expect(() => assertUnreachable(value, 'sym')).toThrow(
      'Unreachable code reached when `sym` = "Symbol(test)"',
    );
  });

  test('handles objects with bigint properties', () => {
    const value = {id: BigInt(456)} as never;
    expect(() => assertUnreachable(value, 'data')).toThrow(
      'Unreachable code reached when `data` = {"id":"456n"}',
    );
  });

  test('handles complex expression descriptions from babel plugin', () => {
    const value = 'unknown_state' as never;
    expect(() => assertUnreachable(value, 'config.settings.mode')).toThrow(
      'Unreachable code reached when `config.settings.mode` = "unknown_state"',
    );
  });

  test('handles array access expression descriptions', () => {
    const value = 99 as never;
    expect(() => assertUnreachable(value, 'items[0].type')).toThrow(
      'Unreachable code reached when `items[0].type` = 99',
    );
  });

  test('handles call expression descriptions', () => {
    const value = {invalid: true} as never;
    expect(() => assertUnreachable(value, 'getStatus(...)')).toThrow(
      'Unreachable code reached when `getStatus(...)` = {"invalid":true}',
    );
  });

  test('handles function call result as value', () => {
    const getValue = () => ({computed: true, status: 'error'});
    expect(() => assertUnreachable(getValue() as never, 'result')).toThrow(
      'Unreachable code reached when `result` = {"computed":true,"status":"error"}',
    );
  });

  test('handles deeply nested object as value', () => {
    const value = {
      level1: {
        level2: {
          level3: {
            data: 'deep',
          },
        },
      },
    } as never;
    expect(() => assertUnreachable(value, 'nested')).toThrow(
      'Unreachable code reached when `nested` = {"level1":{"level2":{"level3":{"data":"deep"}}}}',
    );
  });

  test('handles array as value', () => {
    const value = [1, 2, 3, 'mixed', {obj: true}] as never;
    expect(() => assertUnreachable(value, 'arr')).toThrow(
      'Unreachable code reached when `arr` = [1,2,3,"mixed",{"obj":true}]',
    );
  });

  test('handles inline literal object', () => {
    expect(() =>
      assertUnreachable({type: 'UNKNOWN', payload: null} as never, 'action'),
    ).toThrow(
      'Unreachable code reached when `action` = {"type":"UNKNOWN","payload":null}',
    );
  });

  test('handles function value', () => {
    const value = function myFunc() {} as never;
    expect(() => assertUnreachable(value, 'fn')).toThrow(
      'Unreachable code reached when `fn` = "[function myFunc]"',
    );
  });

  test('handles anonymous function value', () => {
    // Use IIFE to create a truly anonymous function (variable assignment names it)
    const value = (() => () => {})() as never;
    expect(() => assertUnreachable(value, 'fn')).toThrow(
      'Unreachable code reached when `fn` = "[function anonymous]"',
    );
  });
});
