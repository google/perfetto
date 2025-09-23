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

import {buildUrlSearchParams, parseUrlSearchParams} from './route_parser';

describe('parseUrlSearchParams', () => {
  it('handles empty params', () => {
    const params = new URLSearchParams('');
    expect(parseUrlSearchParams(params)).toEqual({});
  });

  it('handles single value params', () => {
    const params = new URLSearchParams('foo=bar&baz=qux');
    expect(parseUrlSearchParams(params)).toEqual({
      foo: 'bar',
      baz: 'qux',
    });
  });

  it('handles repeated params', () => {
    const params = new URLSearchParams('foo=bar&foo=baz');
    expect(parseUrlSearchParams(params)).toEqual({
      foo: ['bar', 'baz'],
    });
  });

  it('handles valueless params as true', () => {
    const params = new URLSearchParams('foo&bar=baz');
    expect(parseUrlSearchParams(params)).toEqual({
      foo: true,
      bar: 'baz',
    });
  });

  it('handles true string as boolean', () => {
    const params = new URLSearchParams('foo=true&bar=baz');
    expect(parseUrlSearchParams(params)).toEqual({
      foo: true,
      bar: 'baz',
    });
  });

  it('handles false string as boolean', () => {
    const params = new URLSearchParams('foo=false&bar=baz');
    expect(parseUrlSearchParams(params)).toEqual({
      foo: false,
      bar: 'baz',
    });
  });

  it('handles mixed params', () => {
    const params = new URLSearchParams('a=1&b&c=2&c=3&d=true&e=false');
    expect(parseUrlSearchParams(params)).toEqual({
      a: '1',
      b: true,
      c: ['2', '3'],
      d: true,
      e: false,
    });
  });

  it('handles valueless repeated params', () => {
    const params = new URLSearchParams('foo&foo');
    expect(parseUrlSearchParams(params)).toEqual({
      foo: [true, true],
    });
  });

  it('handles mixed valueless and valued repeated params', () => {
    const params = new URLSearchParams('foo&foo=bar');
    expect(parseUrlSearchParams(params)).toEqual({
      foo: [true, 'bar'],
    });
  });

  it('handles mixed booleans in repeated params', () => {
    const params = new URLSearchParams('foo=true&foo=bar&foo=false');
    expect(parseUrlSearchParams(params)).toEqual({
      foo: [true, 'bar', false],
    });
  });
});

describe('buildUrlSearchParams', () => {
  it('handles empty params', () => {
    expect(buildUrlSearchParams({})).toEqual('');
  });

  it('handles single value params', () => {
    expect(buildUrlSearchParams({foo: 'bar', baz: 'qux'})).toEqual(
      'baz=qux&foo=bar',
    );
  });

  it('handles repeated params', () => {
    expect(buildUrlSearchParams({foo: ['bar', 'baz']})).toEqual(
      'foo=bar&foo=baz',
    );
  });

  it('handles valueless params as true', () => {
    expect(buildUrlSearchParams({foo: true, bar: 'baz'})).toEqual(
      'bar=baz&foo',
    );
  });

  it('handles boolean false', () => {
    expect(buildUrlSearchParams({foo: false, bar: 'baz'})).toEqual(
      'bar=baz&foo=false',
    );
  });

  it('handles mixed params', () => {
    expect(
      buildUrlSearchParams({
        a: '1',
        b: true,
        c: ['2', '3'],
        d: true,
        e: false,
      }),
    ).toEqual('a=1&b&c=2&c=3&d&e=false');
  });

  it('handles valueless repeated params', () => {
    expect(buildUrlSearchParams({foo: [true, true]})).toEqual('foo&foo');
  });

  it('handles mixed valueless and valued repeated params', () => {
    expect(buildUrlSearchParams({foo: [true, 'bar']})).toEqual('foo&foo=bar');
  });

  it('handles mixed booleans in repeated params', () => {
    expect(buildUrlSearchParams({foo: [true, 'bar', false]})).toEqual(
      'foo&foo=bar&foo=false',
    );
  });

  it('handles special characters', () => {
    expect(buildUrlSearchParams({a: ' ', b: '&', c: ['=', '?']})).toEqual(
      'a=%20&b=%26&c=%3D&c=%3F',
    );
  });
});
