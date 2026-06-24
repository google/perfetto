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

export type NavState =
  | {view: 'overview'; params: Record<string, never>}
  | {view: 'classes'; params: {rootClass?: string}}
  | {view: 'dominators'; params: Record<string, never>}
  | {view: 'objects'; params: {cls?: string}}
  | {view: 'object'; params: {id: number; label?: string}}
  | {view: 'bitmaps'; params: {id?: number; filterKey?: string}}
  | {view: 'strings'; params: {q?: string}}
  | {view: 'arrays'; params: {arrayHash?: string}}
  | {
      view: 'flamegraph-objects';
      params: {pathHashes?: string; isDominator?: boolean};
    }
  | {view: 'flamegraph'; params: Record<string, never>}
  | {view: 'callstack'; params: Record<string, never>};

export type NavView = NavState['view'];

// A `key=value` query fragment (URI-encoded), or '' when the value is absent.
function queryParam(key: string, value?: string): string {
  return value ? `${key}=${encodeURIComponent(value)}` : '';
}

// `base`, optionally suffixed with `_<value>` (URI-encoded; a no-op for hex ids).
function pathSegment(base: string, value?: string): string {
  return value ? `${base}_${encodeURIComponent(value)}` : base;
}

// The path and query (no leading '?') for a nav state. Some views encode a param
// in the path (object id, bitmap id, flamegraph tab identity); others carry it
// in the query. stateToSubpage composes the two; stateToPath exposes just the
// path.
function stateToParts(state: NavState): {path: string; query: string} {
  switch (state.view) {
    case 'overview':
      return {path: '', query: ''};
    case 'classes':
      return {
        path: 'classes',
        query: queryParam('root', state.params.rootClass),
      };
    case 'dominators':
      return {path: 'dominators', query: ''};
    case 'objects':
      return {path: 'objects', query: queryParam('cls', state.params.cls)};
    case 'object':
      return {
        path: pathSegment('object', `0x${state.params.id.toString(16)}`),
        query: '',
      };
    case 'bitmaps':
      return {
        path: pathSegment(
          'bitmaps',
          state.params.id !== undefined
            ? `0x${state.params.id.toString(16)}`
            : undefined,
        ),
        query: queryParam('fk', state.params.filterKey),
      };
    case 'strings':
      return {path: 'strings', query: queryParam('q', state.params.q)};
    case 'arrays':
      return {path: 'arrays', query: queryParam('ah', state.params.arrayHash)};
    case 'flamegraph-objects': {
      // The router strips query params from the live route; the tab identity
      // goes in the path.
      const {pathHashes, isDominator} = state.params;
      if (pathHashes === undefined) {
        return {path: 'flamegraph_objects', query: ''};
      }
      const flag = isDominator ? '1' : '0';
      return {
        path: `flamegraph_objects_${flag}_${encodeURIComponent(pathHashes)}`,
        query: '',
      };
    }
    case 'flamegraph':
      return {path: 'flamegraph', query: ''};
    case 'callstack':
      return {path: 'callstack', query: ''};
  }
}

export function stateToPath(state: NavState): string {
  return stateToParts(state).path;
}

export function stateToSubpage(state: NavState): string {
  const {path, query} = stateToParts(state);
  return query ? `${path}?${query}` : path;
}

export function subpageToState(subpage: string | undefined): NavState {
  if (!subpage) return {view: 'overview', params: {}};

  const [path, queryStr] = subpage.split('?', 2);
  const sp = new URLSearchParams(queryStr ?? '');

  // Parse view_param format (e.g. "object_0x123", "flamegraph_objects_1_a,b").
  // Views with underscores: "flamegraph_objects" is the only multi-word view.
  let view: string;
  let param: string;
  if (path.startsWith('flamegraph_objects')) {
    view = 'flamegraph-objects';
    param = path.slice('flamegraph_objects'.length + 1) || '';
  } else {
    const idx = path.indexOf('_');
    view = idx === -1 ? path : path.slice(0, idx);
    param = idx === -1 ? '' : path.slice(idx + 1);
  }

  switch (view) {
    case '':
    case 'overview':
      return {view: 'overview', params: {}};
    case 'classes': {
      const root = sp.get('root') ?? undefined;
      return {view: 'classes', params: root ? {rootClass: root} : {}};
    }
    case 'dominators':
      return {view: 'dominators', params: {}};
    case 'objects': {
      const cls = sp.get('cls') ?? undefined;
      return {view: 'objects', params: cls ? {cls} : {}};
    }
    case 'object': {
      const raw = param || '0';
      const id = raw.startsWith('0x')
        ? parseInt(raw.slice(2), 16)
        : parseInt(raw, 10);
      return {view: 'object', params: {id: id || 0}};
    }
    case 'bitmaps': {
      const selectedId = param.startsWith('0x')
        ? parseInt(param.slice(2), 16)
        : param
          ? parseInt(param, 10)
          : 0;
      const fk = sp.get('fk') ?? undefined;
      const bitmapParams: {id?: number; filterKey?: string} = {};
      if (selectedId) bitmapParams.id = selectedId;
      if (fk) bitmapParams.filterKey = fk;
      return {view: 'bitmaps', params: bitmapParams};
    }
    case 'strings': {
      const q = sp.get('q') ?? '';
      return {view: 'strings', params: q ? {q} : {}};
    }
    case 'arrays': {
      const ah = sp.get('ah') ?? undefined;
      return {view: 'arrays', params: ah ? {arrayHash: ah} : {}};
    }
    case 'flamegraph-objects': {
      // param is "<dom>_<encoded pathHashes>" (see stateToParts), or empty when
      // no tab is selected.
      if (param === '') return {view: 'flamegraph-objects', params: {}};
      const us = param.indexOf('_');
      const isDominator = us > 0 && param.slice(0, us) === '1';
      const rest = us === -1 ? param : param.slice(us + 1);
      return {
        view: 'flamegraph-objects',
        params: {pathHashes: decodeURIComponent(rest), isDominator},
      };
    }
    case 'flamegraph':
      return {view: 'flamegraph', params: {}};
    case 'callstack':
      return {view: 'callstack', params: {}};
    default:
      return {view: 'overview', params: {}};
  }
}
