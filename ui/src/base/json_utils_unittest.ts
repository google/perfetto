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

import {stringifyJsonWithBigints} from './json_utils';

test('stringifyJsonWithBigints', () => {
  const obj = {foo: 'bar', baz: 123n};
  const expected = '{"foo":"bar","baz":"123"}';
  expect(stringifyJsonWithBigints(obj)).toEqual(expected);
});

test('stringifyJsonWithBigints with space', () => {
  const obj = {foo: 'bar', baz: 123n};
  const expected = `{
  "foo": "bar",
  "baz": "123"
}`;
  expect(stringifyJsonWithBigints(obj, 2)).toEqual(expected);
});
