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
  assertIsArrayBufferView,
  assertIsInstance,
  assertTrue,
  assertUnreachable,
  ensureDefined,
  ensureExists,
  ensureIsInstance,
} from './assert';

class Animal {}
class Dog extends Animal {}

describe('assertTrue', () => {
  test('throws', () => {
    // assertTrue is declared `asserts x`, so strict-boolean-expressions treats
    // the argument as a condition and flags these constant values. Passing them
    // is the whole point of the test, so silence the rule here.
    /* eslint-disable @typescript-eslint/strict-boolean-expressions */
    expect(() => assertTrue(true)).not.toThrow();
    expect(() => assertTrue('string')).not.toThrow();
    expect(() => assertTrue({})).not.toThrow();
    expect(() => assertTrue(new Map())).not.toThrow();

    expect(() => assertTrue(false)).toThrow();
    expect(() => assertTrue(null)).toThrow();
    expect(() => assertTrue(undefined)).toThrow();
    expect(() => assertTrue('')).toThrow();
    expect(() => assertTrue(0)).toThrow();
    expect(() => assertTrue(0n)).toThrow();
    expect(() => assertTrue(NaN)).toThrow();
    expect(() => assertTrue(Number.NaN)).toThrow();
    /* eslint-enable @typescript-eslint/strict-boolean-expressions */
  });

  test('throws with custom message', () => {
    expect(() => assertTrue(false, 'boom')).toThrow('boom');
  });

  test('narrows types', () => {
    // Typescript test - this should compile
    const x: boolean = true;
    assertTrue(x);
    const check: true = x; // compiles only if x narrowed correctly
    check; // Silence unused const warning
  });
});

describe('assertFalse', () => {
  test('throws', () => {
    expect(() => assertFalse(false)).not.toThrow();
    expect(() => assertFalse(null)).not.toThrow();
    expect(() => assertFalse(undefined)).not.toThrow();
    expect(() => assertFalse('')).not.toThrow();
    expect(() => assertFalse(0)).not.toThrow();
    expect(() => assertFalse(0n)).not.toThrow();
    expect(() => assertFalse(NaN)).not.toThrow();
    expect(() => assertFalse(Number.NaN)).not.toThrow();

    expect(() => assertFalse(true)).toThrow();
    expect(() => assertFalse('string')).toThrow();
    expect(() => assertFalse({})).toThrow();
    expect(() => assertFalse(new Map())).toThrow();
  });

  test('throws with custom message', () => {
    expect(() => assertFalse(true, 'boom')).toThrow('boom');
  });

  test('narrows types', () => {
    // Typescript test - this should compile
    const x: boolean = false;
    assertFalse(x);
    const check: false = x; // compiles only if x narrowed correctly
    check; // Silence unused const warning
  });
});

describe('ensureExists', () => {
  test('returns the value when present', () => {
    expect(ensureExists('a')).toBe('a');
    const obj = {};
    expect(ensureExists(obj)).toBe(obj);
  });

  test('passes through falsy non-nullish values', () => {
    expect(ensureExists(0)).toBe(0);
    expect(ensureExists('')).toBe('');
    expect(ensureExists(false)).toBe(false);
  });

  test('throws on null and undefined', () => {
    expect(() => ensureExists(null)).toThrow();
    expect(() => ensureExists(undefined)).toThrow();
  });

  test('throws with custom message', () => {
    expect(() => ensureExists(null, 'boom')).toThrow('boom');
  });

  test('narrows the return type', () => {
    const x: string | null | undefined = 'a';
    const check: string = ensureExists(x); // compiles only if narrowed
    check;
  });
});

describe('assertExists', () => {
  test('does not throw when present', () => {
    expect(() => assertExists('a')).not.toThrow();
    expect(() => assertExists(0)).not.toThrow();
    expect(() => assertExists('')).not.toThrow();
    expect(() => assertExists(false)).not.toThrow();
  });

  test('throws on null and undefined', () => {
    expect(() => assertExists(null)).toThrow();
    expect(() => assertExists(undefined)).toThrow();
  });

  test('throws with custom message', () => {
    expect(() => assertExists(undefined, 'boom')).toThrow('boom');
  });

  test('narrows the value in place', () => {
    const x: string | null | undefined = 'a';
    assertExists(x);
    const check: string = x; // compiles only if null|undefined removed
    check;
  });
});

describe('ensureDefined', () => {
  test('returns the value when defined', () => {
    expect(ensureDefined('a')).toBe('a');
    expect(ensureDefined(0)).toBe(0);
  });

  test('passes null through (only undefined is rejected)', () => {
    expect(ensureDefined(null)).toBeNull();
  });

  test('throws on undefined', () => {
    expect(() => ensureDefined(undefined)).toThrow();
  });

  test('throws with custom message', () => {
    expect(() => ensureDefined(undefined, 'boom')).toThrow('boom');
  });

  test('narrows the return type but keeps null', () => {
    const x: string | null | undefined = null;
    const check: string | null = ensureDefined(x); // undefined removed
    check;
  });
});

describe('assertDefined', () => {
  test('does not throw for defined values, including null', () => {
    expect(() => assertDefined('a')).not.toThrow();
    expect(() => assertDefined(null)).not.toThrow();
    expect(() => assertDefined(0)).not.toThrow();
  });

  test('throws on undefined', () => {
    expect(() => assertDefined(undefined)).toThrow();
  });

  test('throws with custom message', () => {
    expect(() => assertDefined(undefined, 'boom')).toThrow('boom');
  });

  test('narrows the value in place but keeps null', () => {
    const x: string | null | undefined = null;
    assertDefined(x);
    const check: string | null = x; // only undefined removed
    check;
  });
});

describe('ensureIsInstance', () => {
  test('returns the value when it matches', () => {
    const dog = new Dog();
    expect(ensureIsInstance(dog, Dog)).toBe(dog);
    // Subclass instance is also an instance of the base class.
    expect(ensureIsInstance(dog, Animal)).toBe(dog);
  });

  test('throws when it does not match', () => {
    expect(() => ensureIsInstance(new Animal(), Dog)).toThrow();
    expect(() => ensureIsInstance({}, Dog)).toThrow();
    expect(() => ensureIsInstance(null, Dog)).toThrow();
  });

  test('default message names the class', () => {
    expect(() => ensureIsInstance({}, Dog)).toThrow('Dog');
  });

  test('throws with custom message', () => {
    expect(() => ensureIsInstance({}, Dog, 'boom')).toThrow('boom');
  });

  test('narrows the return type', () => {
    const x: unknown = new Dog();
    const check: Dog = ensureIsInstance(x, Dog); // compiles only if narrowed
    check;
  });
});

describe('assertIsInstance', () => {
  test('does not throw when it matches', () => {
    expect(() => assertIsInstance(new Dog(), Dog)).not.toThrow();
    expect(() => assertIsInstance(new Dog(), Animal)).not.toThrow();
  });

  test('throws when it does not match', () => {
    expect(() => assertIsInstance(new Animal(), Dog)).toThrow();
    expect(() => assertIsInstance({}, Dog)).toThrow();
  });

  test('throws with custom message', () => {
    expect(() => assertIsInstance({}, Dog, 'boom')).toThrow('boom');
  });

  test('narrows the value in place', () => {
    const x: unknown = new Dog();
    assertIsInstance(x, Dog);
    const check: Dog = x; // compiles only if narrowed to Dog
    check;
  });
});

describe('assertUnreachable', () => {
  test('always throws', () => {
    expect(() => assertUnreachable('x' as never)).toThrow();
  });

  test('throws with custom message', () => {
    expect(() => assertUnreachable('x' as never, 'boom')).toThrow('boom');
  });

  test('enforces exhaustive switches at compile time', () => {
    type Color = 'red' | 'green';
    const name = (c: Color): string => {
      switch (c) {
        case 'red':
          return 'red';
        case 'green':
          return 'green';
        default:
          // Compiles only if every Color member is handled above; adding a new
          // member would make `c` not `never` and fail to compile here.
          return assertUnreachable(c);
      }
    };
    expect(name('red')).toBe('red');
  });
});

describe('assertIsArrayBufferView', () => {
  test('does not throw for an ArrayBuffer-backed view', () => {
    const view = new Uint8Array(new ArrayBuffer(8));
    expect(() => assertIsArrayBufferView(view)).not.toThrow();
  });

  test('throws for a SharedArrayBuffer-backed view', () => {
    const view = new Uint8Array(new SharedArrayBuffer(8));
    expect(() => assertIsArrayBufferView(view)).toThrow();
  });
});
