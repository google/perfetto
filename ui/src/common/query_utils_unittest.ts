// Copyright (C) 2022 The Android Open Source Project
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

import {escapeGlob, escapeQuery, escapeSearchQuery} from './query_utils';

test('escapeQuery', () => {
  expect(escapeQuery(``)).toEqual(`''`);
  expect(escapeQuery(`'`)).toEqual(`''''`);
  expect(escapeQuery(`hello`)).toEqual(`'hello'`);
  expect(escapeQuery('foo\'bar')).toEqual(`'foo''bar'`);
  expect(escapeQuery('*_*')).toEqual(`'[*]_[*]'`);
  expect(escapeQuery('[]?')).toEqual(`'[[]][?]'`);
});

test('escapeSearchQuery', () => {
  expect(escapeSearchQuery(``)).toEqual(`'**'`);
  expect(escapeSearchQuery(`hello`)).toEqual(`'*[hH][eE][lL][lL][oO]*'`);
  expect(escapeSearchQuery('a\'b')).toEqual(`'*[aA]''[bB]*'`);
  expect(escapeSearchQuery('*_*')).toEqual(`'*[*]_[*]*'`);
  expect(escapeSearchQuery('[]?')).toEqual(`'*[[]][?]*'`);
});

test('escapeGlob', () => {
  expect(escapeGlob(``)).toEqual(`'**'`);
  expect(escapeGlob(`'`)).toEqual(`'*''*'`);
  expect(escapeGlob(`hello`)).toEqual(`'*hello*'`);
  expect(escapeGlob('foo\'bar')).toEqual(`'*foo''bar*'`);
  expect(escapeGlob('*_*')).toEqual(`'**_**'`);
  expect(escapeGlob('[]?')).toEqual(`'*[]?*'`);
});
