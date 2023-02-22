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

import {constraintsToQueryFragment} from './sql_utils';

// Clean up repeated whitespaces to allow for easier testing.
function normalize(s: string): string {
  return s.replace(/\s+/g, ' ');
}

test('constraintsToQueryFragment: where', () => {
  expect(normalize(constraintsToQueryFragment({
    where: ['ts > 1000', 'dur != 0'],
  }))).toEqual('WHERE ts > 1000 and dur != 0');
});

test('constraintsToQueryFragment: order by', () => {
  expect(normalize(constraintsToQueryFragment({
    orderBy: [{fieldName: 'name'}, {fieldName: 'count', direction: 'DESC'}],
  }))).toEqual('ORDER BY name, count DESC');
});

test('constraintsToQueryFragment: limit', () => {
  expect(normalize(constraintsToQueryFragment({limit: 3}))).toEqual('LIMIT 3');
});

test('constraintsToQueryFragment: all', () => {
  expect(normalize(constraintsToQueryFragment({
    where: ['id != 1'],
    orderBy: [{fieldName: 'ts'}],
    limit: 1,
  }))).toEqual('WHERE id != 1 ORDER BY ts LIMIT 1');
});
