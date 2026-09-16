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

import {describe, expect, it} from 'vitest';
import {stateToPath, stateToSubpage, subpageToState} from './nav_state';

describe('nav_state', () => {
  it('defaults to overview when defaultView is omitted or overview', () => {
    expect(subpageToState(undefined)).toEqual({
      view: 'overview',
      params: {},
    });
    expect(subpageToState('')).toEqual({
      view: 'overview',
      params: {},
    });
    expect(subpageToState(undefined, 'overview')).toEqual({
      view: 'overview',
      params: {},
    });
    expect(subpageToState('', 'overview')).toEqual({
      view: 'overview',
      params: {},
    });
  });

  it('defaults to flamegraph when defaultView is flamegraph', () => {
    expect(subpageToState(undefined, 'flamegraph')).toEqual({
      view: 'flamegraph',
      params: {},
    });
    expect(subpageToState('', 'flamegraph')).toEqual({
      view: 'flamegraph',
      params: {},
    });
  });

  it('parses explicit views regardless of defaultView', () => {
    expect(subpageToState('overview', 'flamegraph')).toEqual({
      view: 'overview',
      params: {},
    });
    expect(subpageToState('flamegraph', 'overview')).toEqual({
      view: 'flamegraph',
      params: {},
    });
    expect(subpageToState('classes?root=Foo', 'flamegraph')).toEqual({
      view: 'classes',
      params: {rootClass: 'Foo'},
    });
    expect(subpageToState('objects_java.lang.String', 'flamegraph')).toEqual({
      view: 'objects',
      params: {cls: 'java.lang.String'},
    });
  });

  it('serializes overview and flamegraph states to path and subpage', () => {
    expect(stateToPath({view: 'overview', params: {}})).toBe('overview');
    expect(stateToSubpage({view: 'overview', params: {}})).toBe('overview');
    expect(stateToPath({view: 'flamegraph', params: {}})).toBe('flamegraph');
    expect(stateToSubpage({view: 'flamegraph', params: {}})).toBe('flamegraph');
  });
});
