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

import {convertArgsToTree} from './args_parser';

test('parseArgs', () => {
  const input = new Map<string, string>([
    ['simple_key', 'simple_value'],
    ['thing.key', 'value'],
    ['thing.point[0].x', '10'],
    ['thing.point[0].y', '20'],
    ['thing.point[1].x', '0'],
    ['thing.point[1].y', '-10'],
    ['foo.bar.foo.bar', 'baz'],
  ]);

  const result = convertArgsToTree(input);

  expect(result).toEqual(
      [
        {key: 'simple_key', path: 'simple_key', value: 'simple_value'},
        {
          key: 'thing',
          children: [
            {key: 'key', path: 'thing.key', value: 'value'},
            {
              key: 'point',
              children: [
                {
                  key: 0,
                  children: [
                    {key: 'x', path: 'thing.point[0].x', value: '10'},
                    {key: 'y', path: 'thing.point[0].y', value: '20'},
                  ],
                },
                {
                  key: 1,
                  children: [
                    {key: 'x', path: 'thing.point[1].x', value: '0'},
                    {key: 'y', path: 'thing.point[1].y', value: '-10'},
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
                    {key: 'bar', path: 'foo.bar.foo.bar', value: 'baz'},
                  ],
                },
              ],
            },
          ],
        },
      ],
  );
});
