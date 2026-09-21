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
export type DefaultNavView = 'overview' | 'flamegraph';

function defaultNavState(view: DefaultNavView): NavState {
  if (view === 'flamegraph') {
    return {view: 'flamegraph', params: {}};
  }
  return {view: 'overview', params: {}};
}

// A `key=value` query fragment (URI-encoded), or '' when the value is absent.
function queryParam(key: string, value?: string): string {
  return value ? `${key}=${encodeURIComponent(value)}` : '';
}

// The path and query (no leading '?') for a nav state. Some views encode a param
// in the path (object id, bitmap id, flamegraph tab identity); others carry it
// in the query. stateToSubpage composes the two; stateToPath exposes just the
// path.
function stateToParts(state: NavState): {path: string; query: string} {
  switch (state.view) {
    case 'overview':
      return {path: 'overview', query: ''};
    case 'classes':
      return {
        path: 'classes',
        query: queryParam('root', state.params.rootClass),
      };
    case 'dominators':
      return {path: 'dominators', query: ''};
    case 'objects':
      // Class filter in the path, not the query: the router drops query params.
      return {
        path:
          state.params.cls !== undefined
            ? `objects/${encodeURIComponent(state.params.cls)}`
            : 'objects',
        query: '',
      };
    case 'object':
      return {
        path: `object/0x${state.params.id.toString(16)}`,
        query: '',
      };
    case 'bitmaps':
      return {
        path:
          state.params.id !== undefined
            ? `bitmaps/0x${state.params.id.toString(16)}`
            : 'bitmaps',
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
        path: `flamegraph_objects/${flag}/${encodeURIComponent(pathHashes)}`,
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

export function subpageToState(
  subpage: string | undefined,
  defaultView: DefaultNavView = 'overview',
): NavState {
  if (!subpage) return defaultNavState(defaultView);

  const [path, queryStr] = subpage.split('?', 2);
  const sp = new URLSearchParams(queryStr ?? '');

  // Parse the slash-separated path (e.g. "object/0x123",
  // "flamegraph_objects/1/a,b"). The first segment is the view name; the
  // remaining segments carry its params.
  const [view = '', ...paramSegments] = path.split('/');
  const param = paramSegments[0] ?? '';

  switch (view) {
    case '':
      return defaultNavState(defaultView);
    case 'overview':
      return {view: 'overview', params: {}};
    case 'classes': {
      const root = sp.get('root') ?? undefined;
      return {view: 'classes', params: root ? {rootClass: root} : {}};
    }
    case 'dominators':
      return {view: 'dominators', params: {}};
    case 'objects': {
      const cls = param ? decodeURIComponent(param) : undefined;
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
    case 'flamegraph_objects': {
      // Path is "flamegraph_objects/<dom>/<encoded pathHashes>" (see
      // stateToParts); missing segments mean no tab is selected.
      const [dom, hashes] = paramSegments;
      if (dom === undefined || hashes === undefined) {
        return {view: 'flamegraph-objects', params: {}};
      }
      return {
        view: 'flamegraph-objects',
        params: {
          pathHashes: decodeURIComponent(hashes),
          isDominator: dom === '1',
        },
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
