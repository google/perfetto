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

import {classNames} from './classnames';

test('classnames', () => {
  expect(classNames('foo', 'bar')).toEqual('foo bar');
  expect(classNames('foo', '', 'bar')).toEqual('foo bar');
  expect(classNames(false, 'foo', 'bar')).toEqual('foo bar');
  expect(classNames(undefined, 'foo', 'bar')).toEqual('foo bar');
  expect(classNames('foo', 'bar', ['baz', 'qux'])).toEqual('foo bar baz qux');
  expect(classNames('foo bar', 'baz')).toEqual('foo bar baz');
});

test('example usecase with flags', () => {
  const foo = true;
  const bar = false;
  const baz = true;
  expect(classNames(foo && 'foo', bar && 'bar', baz && 'baz')).toEqual(
    'foo baz',
  );
});

test('example usecase with possibly undefined classnames', () => {
  let fooClass: string | undefined;
  const barClass = 'bar';
  expect(classNames(fooClass, barClass)).toEqual('bar');
});
