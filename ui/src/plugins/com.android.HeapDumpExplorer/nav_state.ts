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

// Internal navigation state for the Heapdump Explorer plugin.

import m from 'mithril';

export type NavState =
  | {view: 'overview'; params: Record<string, never>}
  | {view: 'classes'; params: {rootClass?: string}}
  | {view: 'dominators'; params: Record<string, never>}
  | {view: 'objects'; params: {cls?: string}}
  | {view: 'object'; params: {id: number; label?: string}}
  | {view: 'bitmaps'; params: {id?: number; filterKey?: string}}
  | {view: 'strings'; params: {q?: string}}
  | {view: 'arrays'; params: {arrayHash?: string}}
  | {view: 'flamegraph-objects'; params: {name?: string}};

export function stateToSubpage(state: NavState): string {
  switch (state.view) {
    case 'overview':
      return '';
    case 'classes': {
      const root = state.params.rootClass;
      return root ? `classes?root=${encodeURIComponent(root)}` : 'classes';
    }
    case 'dominators':
      return 'dominators';
    case 'objects': {
      const cls = state.params.cls;
      return cls ? `objects?cls=${encodeURIComponent(cls)}` : 'objects';
    }
    case 'object':
      return `object_0x${state.params.id.toString(16)}`;
    case 'bitmaps': {
      const id = state.params.id;
      const fk = state.params.filterKey;
      const base =
        id !== undefined ? `bitmaps_0x${id.toString(16)}` : 'bitmaps';
      return fk ? `${base}?fk=${encodeURIComponent(fk)}` : base;
    }
    case 'strings': {
      const q = state.params.q;
      return q ? `strings?q=${encodeURIComponent(q)}` : 'strings';
    }
    case 'arrays': {
      const ah = state.params.arrayHash;
      return ah ? `arrays?ah=${encodeURIComponent(ah)}` : 'arrays';
    }
    case 'flamegraph-objects': {
      const n = state.params.name;
      return n
        ? `flamegraph_objects_${encodeURIComponent(n)}`
        : 'flamegraph_objects';
    }
  }
}

export function subpageToState(subpage: string | undefined): NavState {
  if (!subpage) return {view: 'overview', params: {}};

  const [path, queryStr] = subpage.split('?', 2);
  const sp = new URLSearchParams(queryStr ?? '');

  // Parse view_param format (e.g. "object_0x123", "flamegraph_objects_name").
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
      const n = param ? decodeURIComponent(param) : undefined;
      return {view: 'flamegraph-objects', params: n ? {name: n} : {}};
    }
    default:
      return {view: 'overview', params: {}};
  }
}

export let nav: NavState = {view: 'overview', params: {}};

let navigateCallback: ((subpage: string) => void) | undefined;

export function setNavigateCallback(
  cb: ((subpage: string) => void) | undefined,
): void {
  navigateCallback = cb;
}

export function navigate(
  v: NavState['view'],
  p: Record<string, unknown> = {},
): void {
  const state = {view: v, params: p} as NavState;
  nav = state;
  navigateCallback?.(stateToSubpage(state));
  m.redraw();
}

// Clear a single nav param without pushing a history entry.
// Used after consuming a one-shot param (e.g. a filter from overview).
export function clearNavParam(key: string): void {
  const params = {...(nav.params as Record<string, unknown>)};
  delete params[key];
  nav = {view: nav.view, params} as NavState;
}

export function syncFromSubpage(subpage: string | undefined): void {
  if (subpage?.startsWith('/')) subpage = subpage.slice(1);
  // Compare path-only: Perfetto's router strips query params from subpage.
  const currentSubpage = stateToSubpage(nav);
  const currentPath = currentSubpage.split('?')[0];
  const incomingPath = (subpage ?? '').split('?')[0];
  if (incomingPath !== currentPath) {
    nav = subpageToState(subpage);
  }
}

// Shared flag for sidebar sub-item visibility (readable by sidebar callbacks).
export let hasReadyHprofSession = false;
export function setHasReadyHprofSession(v: boolean): void {
  hasReadyHprofSession = v;
}
