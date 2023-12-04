// Copyright (C) 2021 The Android Open Source Project
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
  arrayOf,
  num,
  oneOf,
  optStr,
  record,
  requiredStr,
  runValidator,
  ValidatedType,
  ValidationError,
} from './validators';

const colors = ['RED', 'GREEN', 'BLUE'] as const;

type Color = typeof colors[number];

const point = record({
  id: requiredStr,
  color: oneOf<Color>(colors, 'RED'),
  x: num(),
  y: num(1),
  properties: record({mass: num(10)}),
});

type Point = ValidatedType<typeof point>;

const nested =
    record({deeply: record({nested: record({array: arrayOf(point)})})});

test('validator ensures presence of required fields', () => {
  expect(() => {
    runValidator(point, {});
  }).toThrow(ValidationError);
});

test('validator ensures correct type of required fields', () => {
  expect(() => {
    runValidator(point, {id: 0});
  }).toThrow(ValidationError);
});

test('validator fills default values', () => {
  const p: Point = runValidator(point, {id: 'test'}).result;

  expect(p.color).toEqual('RED');
  expect(p.x).toEqual(0);
  expect(p.y).toEqual(1);
  expect(p.properties.mass).toEqual(10);
});

test('validator uses provided values', () => {
  const p: Point =
      runValidator(
          point,
          {id: 'test', x: 100, y: 200, color: 'GREEN', properties: {mass: 20}})
          .result;

  expect(p.color).toEqual('GREEN');
  expect(p.x).toEqual(100);
  expect(p.y).toEqual(200);
  expect(p.properties.mass).toEqual(20);
});

test('validator keeps information about extra and invalid keys', () => {
  const result = runValidator(point, {
    id: 'test',
    x: 'should not be a string',
    extra: 'should not be here',
    properties: {mass: 'should be a number', weight: 'should not be here'},
  });

  expect(result.extraKeys).toContain('extra');
  expect(result.extraKeys).toContain('properties.weight');
  expect(result.invalidKeys).toContain('x');
  expect(result.invalidKeys).toContain('properties.mass');
});

test('validator correctly keeps track of path when reporting keys', () => {
  const result = runValidator(nested, {
    extra1: 0,
    deeply: {
      extra2: 1,
      nested: {
        array: [
          {id: 'point1', x: 'should not be a string'},
          {id: 'point2', extra3: 'should not be here'},
        ],
      },
    },
  });

  expect(result.extraKeys).toContain('extra1');
  expect(result.extraKeys).toContain('deeply.extra2');
  expect(result.extraKeys).toContain('deeply.nested.array[1].extra3');
  expect(result.invalidKeys).toContain('deeply.nested.array[0].x');
});


describe('optStr', () => {
  test('it validates undefined', () => {
    const validation = runValidator(optStr, undefined);
    expect(validation.result).toEqual(undefined);
    expect(validation.invalidKeys).toEqual([]);
    expect(validation.extraKeys).toEqual([]);
  });

  test('it validates string', () => {
    const validation = runValidator(optStr, 'foo');
    expect(validation.result).toEqual('foo');
    expect(validation.invalidKeys).toEqual([]);
    expect(validation.extraKeys).toEqual([]);
  });

  test('it reports numbers', () => {
    const validation = runValidator(optStr, 42);
    expect(validation.result).toEqual(undefined);
    expect(validation.invalidKeys).toEqual(['']);
    expect(validation.extraKeys).toEqual([]);
  });
});
