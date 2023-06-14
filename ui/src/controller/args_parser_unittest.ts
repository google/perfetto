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

import {convertArgsToObject, convertArgsToTree} from './args_parser';

const args = [
  {key: 'simple_key', value: 'simple_value'},
  {key: 'thing.key', value: 'value'},
  {key: 'thing.point[0].x', value: 10},
  {key: 'thing.point[0].y', value: 20},
  {key: 'thing.point[1].x', value: 0},
  {key: 'thing.point[1].y', value: -10},
  {key: 'foo.bar.foo.bar', value: 'baz'},
];

describe('convertArgsToTree', () => {
  test('converts example arg set', () => {
    expect(convertArgsToTree(args))
        .toEqual(
            [
              {
                key: 'simple_key',
                value: {key: 'simple_key', value: 'simple_value'},
              },
              {
                key: 'thing',
                children: [
                  {key: 'key', value: {key: 'thing.key', value: 'value'}},
                  {
                    key: 'point',
                    children: [
                      {
                        key: 0,
                        children: [
                          {
                            key: 'x',
                            value: {key: 'thing.point[0].x', value: 10},
                          },
                          {
                            key: 'y',
                            value: {key: 'thing.point[0].y', value: 20},
                          },
                        ],
                      },
                      {
                        key: 1,
                        children: [
                          {
                            key: 'x',
                            value: {key: 'thing.point[1].x', value: 0},
                          },
                          {
                            key: 'y',
                            value: {key: 'thing.point[1].y', value: -10},
                          },
                        ],
                      },
                    ],
                  },
                ],
              },
              {
                key: 'foo',
                children: [
                  {
                    key: 'bar',
                    children: [
                      {
                        key: 'foo',
                        children: [
                          {
                            key: 'bar',
                            value: {key: 'foo.bar.foo.bar', value: 'baz'},
                          },
                        ],
                      },
                    ],
                  },
                ],
              },
            ],
        );
  });

  test('handles value and children in same node', () => {
    const args = [
      {key: 'foo', value: 'foo'},
      {key: 'foo.bar', value: 'bar'},
    ];
    expect(convertArgsToTree(args)).toEqual([
      {
        key: 'foo',
        value: {key: 'foo', value: 'foo'},
        children: [
          {key: 'bar', value: {key: 'foo.bar', value: 'bar'}},
        ],
      },
    ]);
  });

  test('handles mixed key types', () => {
    const args = [
      {key: 'foo[0]', value: 'foo'},
      {key: 'foo.bar', value: 'bar'},
    ];
    expect(convertArgsToTree(args)).toEqual([
      {
        key: 'foo',
        children: [
          {key: 0, value: {key: 'foo[0]', value: 'foo'}},
          {key: 'bar', value: {key: 'foo.bar', value: 'bar'}},
        ],
      },
    ]);
  });

  test('picks latest where duplicate keys exist', () => {
    const args = [
      {key: 'foo', value: 'foo'},
      {key: 'foo', value: 'bar'},
    ];
    expect(convertArgsToTree(args)).toEqual([
      {key: 'foo', value: {key: 'foo', value: 'bar'}},
    ]);
  });

  test('handles sparse arrays', () => {
    const args = [
      {key: 'foo[12]', value: 'foo'},
    ];
    expect(convertArgsToTree(args)).toEqual([
      {
        key: 'foo',
        children: [
          {key: 12, value: {key: 'foo[12]', value: 'foo'}},
        ],
      },
    ]);
  });
});

describe('convertArgsToObject', () => {
  it('converts example arg set', () => {
    expect(convertArgsToObject(args)).toEqual({
      simple_key: 'simple_value',
      thing: {
        key: 'value',
        point: [
          {x: 10, y: 20},
          {x: 0, y: -10},
        ],
      },
      foo: {bar: {foo: {bar: 'baz'}}},
    });
  });

  test('throws on args containing a node with both value and children', () => {
    expect(() => {
      convertArgsToObject([
        {key: 'foo', value: 'foo'},
        {key: 'foo.bar', value: 'bar'},
      ]);
    }).toThrow();
  });

  test('throws on args containing mixed key types', () => {
    expect(() => {
      convertArgsToObject([
        {key: 'foo[0]', value: 'foo'},
        {key: 'foo.bar', value: 'bar'},
      ]);
    }).toThrow();
  });

  test('picks last one where duplicate keys exist', () => {
    const args = [
      {key: 'foo', value: 'foo'},
      {key: 'foo', value: 'bar'},
    ];
    expect(convertArgsToObject(args)).toEqual({foo: 'bar'});
  });

  test('handles sparse arrays', () => {
    const args = [
      {key: 'foo[3]', value: 'foo'},
    ];
    expect(convertArgsToObject(args)).toEqual({
      foo: [
        undefined,
        undefined,
        undefined,
        'foo',
      ],
    });
  });
});
