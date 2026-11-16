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

import {errResult, okResult, unwrapResult} from './result';

describe('unwrapResult', () => {
  test('returns value for okResult', () => {
    expect(unwrapResult(okResult(42))).toBe(42);
    expect(unwrapResult(okResult())).toBeUndefined();
    expect(unwrapResult(okResult(false))).toBe(false);
    expect(unwrapResult(okResult(''))).toBe('');
  });

  test('rethrows Error instances as-is', () => {
    const err = new TypeError('bad input');
    let thrown: unknown;
    try {
      unwrapResult(errResult(err));
    } catch (e) {
      thrown = e;
    }
    expect(thrown).toBe(err);
  });

  test('wraps string errors in an Error', () => {
    expect(() => unwrapResult(errResult('something failed'))).toThrow(
      new Error('something failed'),
    );
  });

  test('wraps non-Error, non-string errors in an Error with cause', () => {
    const errObj = {type: 'not_found', code: 404};
    let thrown: unknown;
    try {
      unwrapResult(errResult(errObj));
    } catch (e) {
      thrown = e;
    }
    expect(thrown).toBeInstanceOf(Error);
    expect((thrown as Error).cause).toBe(errObj);
    expect((thrown as Error).message).toBeTruthy();
  });
});
