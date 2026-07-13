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

import {escapeRegex, escapeRegexEmptyBrackets} from './flamegraph_regex';

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

test('flamegraph_regex.escapeRegexEmptyBrackets', () => {
  // Bare [] is rewritten to match literally.
  expect(escapeRegexEmptyBrackets('byte[]')).toEqual('byte\\[\\]');
  expect(escapeRegexEmptyBrackets('.*Object[] com\\..*')).toEqual(
    '.*Object\\[\\] com\\..*',
  );
  expect(escapeRegexEmptyBrackets('[][]')).toEqual('\\[\\]\\[\\]');

  // Valid regex constructs are left untouched.
  expect(escapeRegexEmptyBrackets('[abc]+')).toEqual('[abc]+');
  expect(escapeRegexEmptyBrackets('\\[]')).toEqual('\\[]');
  expect(escapeRegexEmptyBrackets('\\[\\]')).toEqual('\\[\\]');
  expect(escapeRegexEmptyBrackets('[a[]]')).toEqual('[a[]]');
  expect(escapeRegexEmptyBrackets('[^]]')).toEqual('[^]]');
  expect(escapeRegexEmptyBrackets('plain')).toEqual('plain');
  expect(escapeRegexEmptyBrackets('')).toEqual('');

  // Trailing backslash does not drop characters.
  expect(escapeRegexEmptyBrackets('foo\\')).toEqual('foo\\');
});
