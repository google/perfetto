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

import {escapeRegex, userFilterToRegex} from './flamegraph_regex';

test('flamegraph_regex.escapeRegex', () => {
  expect(escapeRegex('byte[]')).toEqual('byte\\[\\]');
  expect(escapeRegex('java.lang.Object[]')).toEqual(
    'java\\.lang\\.Object\\[\\]',
  );
  expect(escapeRegex('operator()')).toEqual('operator\\(\\)');
  expect(escapeRegex('std::vector<int*>')).toEqual('std::vector<int\\*>');
  expect(escapeRegex('a$b^c|d')).toEqual('a\\$b\\^c\\|d');
  expect(escapeRegex('plain_name')).toEqual('plain_name');
});

test('flamegraph_regex.userFilterToRegex', () => {
  // Bare text is matched literally: all metacharacters escaped.
  expect(userFilterToRegex('MyClass$Nested')).toEqual('MyClass\\$Nested');
  expect(userFilterToRegex('byte[]')).toEqual('byte\\[\\]');
  expect(userFilterToRegex('operator()')).toEqual('operator\\(\\)');
  expect(userFilterToRegex('malloc')).toEqual('malloc');
  expect(userFilterToRegex('')).toEqual('');

  // `/…/` opts into a raw regex: inner pattern used verbatim.
  expect(userFilterToRegex('/alloc.*/')).toEqual('alloc.*');
  expect(userFilterToRegex('/^main$/')).toEqual('^main$');
  expect(userFilterToRegex('//')).toEqual('');

  // A lone slash is not a regex delimiter.
  expect(userFilterToRegex('/')).toEqual('/');
  expect(userFilterToRegex('a/b')).toEqual('a/b');
});
