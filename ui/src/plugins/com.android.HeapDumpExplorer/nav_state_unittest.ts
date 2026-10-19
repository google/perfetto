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
import type {NavState} from './nav_state';

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

  it('round-trips collections states', () => {
    const roundTrips = (state: NavState) =>
      expect(subpageToState(stateToSubpage(state))).toEqual(state);
    expect(stateToSubpage({view: 'collections', params: {}})).toBe(
      'collections',
    );
    roundTrips({view: 'collections', params: {}});
    roundTrips({
      view: 'flamegraph-collections',
      params: {
        pathHashes: '-123,456',
        label: 'mListeners: ArrayList<Foo_Bar>',
        collectionCls: 'java.util.ArrayList',
      },
    });
    roundTrips({
      view: 'flamegraph-collections',
      params: {
        pathHashes: '789',
        label: 'Foo',
        noField: true,
        cls: 'java.util.ArrayList',
      },
    });
    roundTrips({
      view: 'flamegraph-collections',
      params: {
        pathHashes: '789',
        label: 'Foo',
        field: 'com.android.server.am.ProcessRecord$Foo.m_collections$1',
        cls: 'java.util.HashMap',
        collection: 7,
      },
    });
    roundTrips({
      view: 'flamegraph-collections',
      params: {
        pathHashes: '1,-2',
        label: 'a=b_c%d,é [value]',
        field: 'Foo.m%Map=x',
        collection: 0,
      },
    });
  });

  it('parses invalid collections params leniently', () => {
    expect(subpageToState('flamegraph_collections')).toEqual({
      view: 'collections',
      params: {},
    });
    expect(
      subpageToState('flamegraph_collections_pathHashes=1_collection=abc'),
    ).toEqual({view: 'flamegraph-collections', params: {pathHashes: '1'}});
    expect(
      subpageToState('flamegraph_collections_pathHashes=1_label=%E0%A4'),
    ).toEqual({view: 'flamegraph-collections', params: {pathHashes: '1'}});
  });
});
