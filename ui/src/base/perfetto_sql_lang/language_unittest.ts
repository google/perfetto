// Copyright (C) 2025 The Android Open Source Project
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

import {language} from './language';

describe('perfettoSqlLang', () => {
  test('parses simple SELECT statement', () => {
    const code = 'select * from slice limit 100';
    const tree = language.parser.parse(code);
    const cursor = tree.cursor();

    // Walk to root node
    expect(cursor.name).toBe('Program');
    expect(cursor.firstChild()).toBe(true);
    expect(cursor.name).toBe('Statement');
    expect(cursor.firstChild()).toBe(true);
    expect(cursor.name).toBe('SelectStatement');
    expect(cursor.firstChild()).toBe(true);
    expect(cursor.name).toBe('SelectBody');
  });

  test('parses CREATE PERFETTO TABLE', () => {
    const code =
      'create perfetto table my_table as select id from source';
    const tree = language.parser.parse(code);
    const cursor = tree.cursor();

    expect(cursor.name).toBe('Program');
    expect(cursor.firstChild()).toBe(true);
    expect(cursor.name).toBe('Statement');
    expect(cursor.firstChild()).toBe(true);
    expect(cursor.name).toBe('CreatePerfettoTableStatement');
  });

  test('parses INCLUDE PERFETTO MODULE', () => {
    const code = 'include perfetto module android.startup';
    const tree = language.parser.parse(code);
    const cursor = tree.cursor();

    expect(cursor.name).toBe('Program');
    expect(cursor.firstChild()).toBe(true);
    expect(cursor.name).toBe('Statement');
    expect(cursor.firstChild()).toBe(true);
    expect(cursor.name).toBe('IncludeModuleStatement');
  });
});
