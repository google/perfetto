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
  assertFalse,
  assertInstanceOf,
  assertExists,
  assertTrue,
  assertUnreachable,
  checkExists,
} from './assert';

class Cls {}
class SubCls extends Cls {}
class Unrelated {}

function takesString(x: string) {
  void x;
}

function takesNull(x: null) {
  void x;
}

function takesCls(x: Cls) {
  void x;
}

describe('ensureNonNullish', () => {
  test('non-nullish', () => {
    expect(() => {
      const x = 'foo' as string | null | undefined;
      takesString(checkExists(x));
    }).not.toThrow();
  });
  test('null', () => {
    expect(() => {
      const x = null as string | null | undefined;
      takesString(checkExists(x)); // BOOM
    }).toThrow();
  });
  test('undefined', () => {
    expect(() => {
      const x = undefined as string | null | undefined;
      takesString(checkExists(x)); // BOOM
    }).toThrow();
  });
  test('falsy', () => {
    expect(() => {
      checkExists(false);
      checkExists(0);
      checkExists('');
      checkExists(0n);
    }).not.toThrow();
  });
});

describe('assertNonNullish', () => {
  test('string', () => {
    expect(() => {
      const x = 'foo' as string | null | undefined;
      assertExists(x);
      takesString(x);
    }).not.toThrow();
  });
  test('null', () => {
    expect(() => {
      const x = null as string | null | undefined;
      assertExists(x); // BOOM
      takesString(x);
    }).toThrow();
  });
  test('undefined', () => {
    expect(() => {
      const x = undefined as string | null | undefined;
      assertExists(x); // BOOM
      takesString(x);
    }).toThrow();
  });
  test('falsy', () => {
    expect(() => {
      assertExists(false);
      assertExists(0);
      assertExists('');
      assertExists(0n);
    }).not.toThrow();
  });
});

describe('assertDefined', () => {
  test('defined', () => {
    expect(() => {
      const x = 'foo' as string | undefined;
      assertDefined(x);
      takesString(x);
    }).not.toThrow();
  });
  test('null', () => {
    expect(() => {
      const x = null as null | undefined;
      assertDefined(x);
      takesNull(x);
    }).not.toThrow();
  });
  test('undefined', () => {
    expect(() => {
      const x = undefined as string | undefined;
      assertDefined(x); // BOOM
      takesString(x);
    }).toThrow();
  });
  test('falsy', () => {
    expect(() => {
      assertDefined(false);
      assertDefined(0);
      assertDefined('');
      assertDefined(0n);
    }).not.toThrow();
  });
});

describe('assertInstanceOf', () => {
  test('is instance', () => {
    expect(() => {
      const x = new Cls() as unknown;
      assertInstanceOf(x, Cls);
      takesCls(x);
    }).not.toThrow();
  });
  test('is subclass', () => {
    expect(() => {
      const x = new SubCls() as unknown;
      assertInstanceOf(x, Cls);
      takesCls(x);
    }).not.toThrow();
  });
  test('is not instance', () => {
    expect(() => {
      const x = new Unrelated() as unknown;
      assertInstanceOf(x, Cls);
      takesCls(x);
    }).toThrow();
  });
});

describe('assertTrue', () => {
  test('true', () => {
    expect(() => {
      assertTrue(true);
    }).not.toThrow();
  });
  test('false', () => {
    expect(() => {
      assertTrue(false);
    }).toThrow();
  });
});

describe('assertFalse', () => {
  test('false', () => {
    expect(() => {
      assertFalse(false);
    }).not.toThrow();
  });
  test('true', () => {
    expect(() => {
      assertFalse(true);
    }).toThrow();
  });
});

describe('assertUnreachable', () => {
  test('', () => {
    expect(() => {
      assertUnreachable(null as never);
    }).toThrow();
  });
  test('', () => {
    expect(() => {
      const x = 'foo' as const;
      if (x === 'foo') {
        return;
      }
      assertUnreachable(x);
    }).not.toThrow();
  });
});
