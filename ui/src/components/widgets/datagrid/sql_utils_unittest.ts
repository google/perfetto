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
  filterToSql,
  sqlPathMatch,
  sqlPathNotMatch,
  sqlPathsIn,
  sqlPathsNotIn,
  sqlValue,
  toAlias,
} from './sql_utils';

describe('sqlValue', () => {
  test('null', () => {
    expect(sqlValue(null)).toBe('NULL');
  });

  test('string', () => {
    expect(sqlValue('hello')).toBe("'hello'");
  });

  test('string with single quote', () => {
    expect(sqlValue("it's")).toBe("'it''s'");
  });

  test('string with multiple single quotes', () => {
    expect(sqlValue("it's a 'test'")).toBe("'it''s a ''test'''");
  });

  test('number', () => {
    expect(sqlValue(42)).toBe('42');
    expect(sqlValue(3.14)).toBe('3.14');
    expect(sqlValue(-100)).toBe('-100');
  });

  test('bigint', () => {
    expect(sqlValue(BigInt('9007199254740993'))).toBe('9007199254740993');
  });

  test('blob', () => {
    expect(sqlValue(new Uint8Array([0x01, 0x02, 0x03, 0xff]))).toBe(
      "X'010203ff'",
    );
  });
});

describe('toAlias', () => {
  test('simple identifier', () => {
    expect(toAlias('foo')).toBe('"foo"');
  });

  test('identifier with spaces', () => {
    expect(toAlias('foo bar')).toBe('"foo bar"');
  });
});

describe('sqlPathMatch', () => {
  test('single column without null', () => {
    expect(sqlPathMatch(['a'], ['foo'])).toBe("(a = 'foo')");
  });

  test('single column with null', () => {
    expect(sqlPathMatch(['a'], [null])).toBe('(a IS NULL)');
  });

  test('multiple columns without nulls', () => {
    expect(sqlPathMatch(['a', 'b', 'c'], ['foo', 42, 'bar'])).toBe(
      "(a = 'foo' AND b = 42 AND c = 'bar')",
    );
  });

  test('multiple columns with null in middle', () => {
    expect(sqlPathMatch(['a', 'b', 'c'], ['foo', null, 'bar'])).toBe(
      "(a = 'foo' AND b IS NULL AND c = 'bar')",
    );
  });

  test('multiple columns with multiple nulls', () => {
    expect(sqlPathMatch(['a', 'b', 'c'], [null, 'x', null])).toBe(
      "(a IS NULL AND b = 'x' AND c IS NULL)",
    );
  });
});

describe('sqlPathNotMatch', () => {
  test('single column without null', () => {
    expect(sqlPathNotMatch(['a'], ['foo'])).toBe("NOT (a = 'foo')");
  });

  test('single column with null', () => {
    expect(sqlPathNotMatch(['a'], [null])).toBe('NOT (a IS NULL)');
  });

  test('multiple columns with null', () => {
    expect(sqlPathNotMatch(['a', 'b'], ['foo', null])).toBe(
      "NOT (a = 'foo' AND b IS NULL)",
    );
  });
});

describe('sqlPathsIn', () => {
  test('empty paths returns FALSE', () => {
    expect(sqlPathsIn(['a'], [])).toBe('FALSE');
  });

  test('single column single path without null', () => {
    expect(sqlPathsIn(['a'], [['foo']])).toBe("a IN ('foo')");
  });

  test('single column multiple paths without nulls', () => {
    expect(sqlPathsIn(['a'], [['foo'], ['bar'], ['baz']])).toBe(
      "a IN ('foo', 'bar', 'baz')",
    );
  });

  test('single column single path with null', () => {
    expect(sqlPathsIn(['a'], [[null]])).toBe('(a IS NULL)');
  });

  test('single column mixed paths (with and without nulls)', () => {
    expect(sqlPathsIn(['a'], [['foo'], [null], ['bar']])).toBe(
      "(a IN ('foo', 'bar') OR (a IS NULL))",
    );
  });

  test('multiple columns single path without nulls', () => {
    expect(sqlPathsIn(['a', 'b'], [['foo', 42]])).toBe(
      "(a, b) IN (('foo', 42))",
    );
  });

  test('multiple columns multiple paths without nulls', () => {
    expect(
      sqlPathsIn(
        ['a', 'b'],
        [
          ['foo', 1],
          ['bar', 2],
        ],
      ),
    ).toBe("(a, b) IN (('foo', 1), ('bar', 2))");
  });

  test('multiple columns path with null', () => {
    expect(sqlPathsIn(['a', 'b'], [['foo', null]])).toBe(
      "(a = 'foo' AND b IS NULL)",
    );
  });

  test('multiple columns mixed paths', () => {
    expect(
      sqlPathsIn(
        ['a', 'b'],
        [
          ['foo', 1],
          ['bar', null],
          ['baz', 2],
        ],
      ),
    ).toBe("((a, b) IN (('foo', 1), ('baz', 2)) OR (a = 'bar' AND b IS NULL))");
  });

  test('multiple paths all with nulls', () => {
    expect(
      sqlPathsIn(
        ['a', 'b'],
        [
          [null, 1],
          ['bar', null],
        ],
      ),
    ).toBe("((a IS NULL AND b = 1) OR (a = 'bar' AND b IS NULL))");
  });
});

describe('sqlPathsNotIn', () => {
  test('empty paths returns TRUE', () => {
    expect(sqlPathsNotIn(['a'], [])).toBe('TRUE');
  });

  test('single column single path without null', () => {
    expect(sqlPathsNotIn(['a'], [['foo']])).toBe("a NOT IN ('foo')");
  });

  test('single column multiple paths without nulls', () => {
    expect(sqlPathsNotIn(['a'], [['foo'], ['bar']])).toBe(
      "a NOT IN ('foo', 'bar')",
    );
  });

  test('single column single path with null', () => {
    expect(sqlPathsNotIn(['a'], [[null]])).toBe('NOT (a IS NULL)');
  });

  test('single column mixed paths', () => {
    expect(sqlPathsNotIn(['a'], [['foo'], [null], ['bar']])).toBe(
      "a NOT IN ('foo', 'bar') AND NOT (a IS NULL)",
    );
  });

  test('multiple columns single path without nulls', () => {
    expect(sqlPathsNotIn(['a', 'b'], [['foo', 42]])).toBe(
      "(a, b) NOT IN (('foo', 42))",
    );
  });

  test('multiple columns path with null', () => {
    expect(sqlPathsNotIn(['a', 'b'], [['foo', null]])).toBe(
      "NOT (a = 'foo' AND b IS NULL)",
    );
  });

  test('multiple columns mixed paths', () => {
    expect(
      sqlPathsNotIn(
        ['a', 'b'],
        [
          ['foo', 1],
          ['bar', null],
        ],
      ),
    ).toBe(
      "(a, b) NOT IN (('foo', 1)) AND NOT (a = 'bar' AND b IS NULL)",
    );
  });
});

describe('filterToSql', () => {
  test('equality', () => {
    expect(filterToSql({field: 'x', op: '=', value: 'foo'}, 'col')).toBe(
      "col = 'foo'",
    );
  });

  test('inequality', () => {
    expect(filterToSql({field: 'x', op: '!=', value: 42}, 'col')).toBe(
      'col != 42',
    );
  });

  test('less than', () => {
    expect(filterToSql({field: 'x', op: '<', value: 100}, 'col')).toBe(
      'col < 100',
    );
  });

  test('less than or equal', () => {
    expect(filterToSql({field: 'x', op: '<=', value: 100}, 'col')).toBe(
      'col <= 100',
    );
  });

  test('greater than', () => {
    expect(filterToSql({field: 'x', op: '>', value: 100}, 'col')).toBe(
      'col > 100',
    );
  });

  test('greater than or equal', () => {
    expect(filterToSql({field: 'x', op: '>=', value: 100}, 'col')).toBe(
      'col >= 100',
    );
  });

  test('glob', () => {
    expect(filterToSql({field: 'x', op: 'glob', value: '*foo*'}, 'col')).toBe(
      "col GLOB '*foo*'",
    );
  });

  test('not glob', () => {
    expect(
      filterToSql({field: 'x', op: 'not glob', value: '*foo*'}, 'col'),
    ).toBe("col NOT GLOB '*foo*'");
  });

  test('is null', () => {
    expect(filterToSql({field: 'x', op: 'is null'}, 'col')).toBe('col IS NULL');
  });

  test('is not null', () => {
    expect(filterToSql({field: 'x', op: 'is not null'}, 'col')).toBe(
      'col IS NOT NULL',
    );
  });

  test('in', () => {
    expect(filterToSql({field: 'x', op: 'in', value: [1, 2, 3]}, 'col')).toBe(
      'col IN (1, 2, 3)',
    );
  });

  test('not in', () => {
    expect(
      filterToSql({field: 'x', op: 'not in', value: ['a', 'b']}, 'col'),
    ).toBe("col NOT IN ('a', 'b')");
  });
});
