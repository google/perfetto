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
  matchProcessesByName,
  memscopeLiveHref,
  memscopeProcessHref,
  memscopeProductProcessUrl,
  processRouteArg,
} from './process_focus';

describe('processRouteArg', () => {
  it('returns the trimmed name', () => {
    expect(processRouteArg({process: ' com.example.demo '})).toBe(
      'com.example.demo',
    );
  });

  it('ignores missing, empty and boolean values', () => {
    expect(processRouteArg({})).toBeUndefined();
    expect(processRouteArg({process: ''})).toBeUndefined();
    expect(processRouteArg({process: true})).toBeUndefined();
  });
});

describe('memscopeProcessHref', () => {
  it('encodes the name', () => {
    expect(memscopeLiveHref()).toBe('#!/memscope');
    expect(memscopeProcessHref('a b/c')).toBe('#!/memscope?process=a%20b%2Fc');
    expect(memscopeProductProcessUrl('x')).toBe(
      'https://memscope.perfetto.dev/#!/memscope?process=x',
    );
  });
});

describe('matchProcessesByName', () => {
  const names = ['com.foo', 'com.foo:remote', 'COM.BAR', 'system_server'];
  const match = (n: string) => matchProcessesByName(names, n, (x) => x);

  it('prefers exact matches', () => {
    expect(match('com.foo')).toEqual(['com.foo']);
  });

  it('falls back to case-insensitive then substring', () => {
    expect(match('com.bar')).toEqual(['COM.BAR']);
    expect(match('server')).toEqual(['system_server']);
    expect(match('nope')).toEqual([]);
  });
});
