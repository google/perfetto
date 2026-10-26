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

import {escapeRegex, parseUserFilterRegex} from './flamegraph_regex';

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

test('flamegraph_regex.parseUserFilterRegex', () => {
  // Bare text is escaped and matched case-insensitively.
  expect(parseUserFilterRegex('MyClass$Nested')).toEqual({
    pattern: 'MyClass\\$Nested',
    flags: 'i',
  });
  expect(parseUserFilterRegex('byte[]')).toEqual({
    pattern: 'byte\\[\\]',
    flags: 'i',
  });
  expect(parseUserFilterRegex('operator()')).toEqual({
    pattern: 'operator\\(\\)',
    flags: 'i',
  });
  expect(parseUserFilterRegex('malloc')).toEqual({
    pattern: 'malloc',
    flags: 'i',
  });
  expect(parseUserFilterRegex('')).toEqual({pattern: '', flags: 'i'});

  // `/…/` is a case-sensitive raw regex.
  expect(parseUserFilterRegex('/Alloc.*/')).toEqual({
    pattern: 'Alloc.*',
    flags: '',
  });
  expect(parseUserFilterRegex('/^main$/')).toEqual({
    pattern: '^main$',
    flags: '',
  });
  expect(parseUserFilterRegex('//')).toEqual({pattern: '', flags: ''});

  // `/…/i` is a case-insensitive raw regex.
  expect(parseUserFilterRegex('/alloc.*/i')).toEqual({
    pattern: 'alloc.*',
    flags: 'i',
  });
  expect(parseUserFilterRegex('//i')).toEqual({pattern: '', flags: 'i'});

  // A lone or unmatched slash is literal text.
  expect(parseUserFilterRegex('/')).toEqual({pattern: '/', flags: 'i'});
  expect(parseUserFilterRegex('a/b')).toEqual({pattern: 'a/b', flags: 'i'});
});
