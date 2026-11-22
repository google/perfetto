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
  | {view: 'summary'; params: Record<string, never>}
  | {view: 'smaps'; params: Record<string, never>}
  | {view: 'timeline'; params: Record<string, never>}
  | {view: 'native-overview'; params: Record<string, never>}
  | {view: 'native-flamegraph'; params: {q?: string}}
  | {view: 'native-callsites'; params: Record<string, never>}
  | {view: 'native-growth'; params: Record<string, never>}
  | {view: 'file-backed'; params: Record<string, never>}
  | {view: 'graphics'; params: Record<string, never>}
  | {view: 'stacks'; params: Record<string, never>}
  | {view: 'other-mem'; params: Record<string, never>}
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
export type DefaultNavView = 'overview' | 'flamegraph' | 'summary';

export function isOverviewView(view: NavView): boolean {
  return view === 'summary' || view === 'smaps' || view === 'timeline';
}

export function isNativeView(view: NavView): boolean {
  return (
    view === 'native-overview' ||
    view === 'native-flamegraph' ||
    view === 'native-callsites' ||
    view === 'native-growth'
  );
}

export function isCompositionDomainView(view: NavView): boolean {
  return (
    view === 'file-backed' ||
    view === 'graphics' ||
    view === 'stacks' ||
    view === 'other-mem'
  );
}

// Views rendered by the Memory Explorer rather than the Java heap dump views.
export function isProcessView(view: NavView): boolean {
  return (
    isOverviewView(view) || isNativeView(view) || isCompositionDomainView(view)
  );
}

function defaultNavState(view: DefaultNavView): NavState {
  if (view === 'flamegraph') {
    return {view: 'flamegraph', params: {}};
  }
  if (view === 'summary') {
    return {view: 'summary', params: {}};
  }
  return {view: 'overview', params: {}};
}

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
    case 'summary':
      return {path: 'summary', query: ''};
    case 'smaps':
      return {path: 'smaps', query: ''};
    case 'timeline':
      return {path: 'timeline', query: ''};
    case 'native-overview':
      return {path: 'native-overview', query: ''};
    case 'native-flamegraph':
      return {
        path: 'native-flamegraph',
        query: queryParam('q', state.params.q),
      };
    case 'native-callsites':
      return {path: 'native-callsites', query: ''};
    case 'native-growth':
      return {path: 'native-growth', query: ''};
    case 'file-backed':
      return {path: 'file-backed', query: ''};
    case 'graphics':
      return {path: 'graphics', query: ''};
    case 'stacks':
      return {path: 'stacks', query: ''};
    case 'other-mem':
      return {path: 'other-mem', query: ''};
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
      return {path: pathSegment('objects', state.params.cls), query: ''};
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

export function subpageToState(
  subpage: string | undefined,
  defaultView: DefaultNavView = 'overview',
): NavState {
  if (!subpage) return defaultNavState(defaultView);

  const [path, queryStr] = subpage.split('?', 2);
  if (path === undefined) return defaultNavState(defaultView);
  const sp = new URLSearchParams(queryStr ?? '');

  // Parse view_param format (e.g. "object_0x123", "flamegraph_objects_1_a,b").
  // Views with underscores: "flamegraph_objects" is the only multi-word view.
  let view: string;
  let param: string;
  if (path!.startsWith('flamegraph_objects')) {
    view = 'flamegraph-objects';
    param = path!.slice('flamegraph_objects'.length + 1) || '';
  } else {
    const idx = path!.indexOf('_');
    view = idx === -1 ? path! : path!.slice(0, idx);
    param = idx === -1 ? '' : path!.slice(idx + 1);
  }

  switch (view) {
    case '':
      return defaultNavState(defaultView);
    case 'summary':
      return {view: 'summary', params: {}};
    case 'smaps':
      return {view: 'smaps', params: {}};
    case 'timeline':
      return {view: 'timeline', params: {}};
    case 'native-overview':
      return {view: 'native-overview', params: {}};
    case 'native-flamegraph': {
      const q = sp.get('q') ?? '';
      return {view: 'native-flamegraph', params: q ? {q} : {}};
    }
    case 'native-callsites':
      return {view: 'native-callsites', params: {}};
    case 'native-growth':
      return {view: 'native-growth', params: {}};
    case 'file-backed':
      return {view: 'file-backed', params: {}};
    case 'graphics':
      return {view: 'graphics', params: {}};
    case 'stacks':
      return {view: 'stacks', params: {}};
    case 'other-mem':
      return {view: 'other-mem', params: {}};
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

// A Memory Explorer location: the process plus the nav within it.
export interface MemoryLocation {
  readonly upid?: number;
  readonly nav: NavState;
}

// Memory Explorer subpage: `<upid>/<nav subpage>`, e.g. `12/objects_Foo`.
export function memoryLocationToSubpage(loc: MemoryLocation): string {
  if (loc.upid === undefined) return '';
  return `${loc.upid}/${stateToSubpage(loc.nav)}`;
}

export function subpageToMemoryLocation(
  subpage: string | undefined,
): MemoryLocation {
  const sub = (subpage ?? '').replace(/^\/+/, '');
  const slash = sub.indexOf('/');
  const head = slash === -1 ? sub : sub.slice(0, slash);
  const rest = slash === -1 ? '' : sub.slice(slash + 1);
  const upid = /^\d+$/.test(head) ? parseInt(head, 10) : undefined;
  return {upid, nav: subpageToState(rest, 'summary')};
}
