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
  | {view: 'classes'; params: Record<string, never>}
  | {view: 'dominators'; params: Record<string, never>}
  | {view: 'objects'; params: Record<string, never>}
  | {view: 'object'; params: {id: number; label?: string}}
  | {
      view: 'instances';
      params: {className: string; heap: string | null};
    }
  | {view: 'bitmaps'; params: {id?: number; filterKey?: string}}
  | {view: 'strings'; params: {q?: string}}
  | {view: 'flamegraph-objects'; params: {name?: string}};

export function navLabel(state: NavState): string {
  switch (state.view) {
    case 'overview':
      return 'Overview';
    case 'classes':
      return 'Classes';
    case 'dominators':
      return 'Dominators';
    case 'objects':
      return 'Objects';
    case 'object':
      return state.params.label ?? `Object 0x${state.params.id.toString(16)}`;
    case 'instances': {
      const cls = state.params.className;
      const short = cls.includes('.')
        ? cls.slice(cls.lastIndexOf('.') + 1)
        : cls;
      return short || 'Instances';
    }
    case 'bitmaps':
      return 'Bitmaps';
    case 'strings':
      return 'Strings';
    case 'flamegraph-objects': {
      const n = state.params.name;
      return n ? `Flamegraph: ${n}` : 'Flamegraph Objects';
    }
  }
}

export interface BreadcrumbEntry {
  state: NavState;
  label: string;
}

export function makeCrumb(state: NavState): BreadcrumbEntry {
  return {state, label: navLabel(state)};
}

export function stateToSubpage(state: NavState): string {
  switch (state.view) {
    case 'overview':
      return '';
    case 'classes':
      return 'classes';
    case 'dominators':
      return 'dominators';
    case 'objects':
      return 'objects';
    case 'object':
      return `object_0x${state.params.id.toString(16)}`;
    case 'instances': {
      const sp = new URLSearchParams();
      sp.set('class', state.params.className);
      if (state.params.heap) sp.set('heap', state.params.heap);
      return `instances?${sp.toString()}`;
    }
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
    case 'classes':
      return {view: 'classes', params: {}};
    case 'dominators':
      return {view: 'dominators', params: {}};
    case 'objects':
      return {view: 'objects', params: {}};
    case 'object': {
      const raw = param || '0';
      const id = raw.startsWith('0x')
        ? parseInt(raw.slice(2), 16)
        : parseInt(raw, 10);
      return {view: 'object', params: {id: id || 0}};
    }
    case 'instances': {
      const className = sp.get('class') ?? '';
      const heap = sp.get('heap') || null;
      return {view: 'instances', params: {className, heap}};
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
    case 'flamegraph-objects': {
      const n = param ? decodeURIComponent(param) : undefined;
      return {view: 'flamegraph-objects', params: n ? {name: n} : {}};
    }
    default:
      return {view: 'overview', params: {}};
  }
}

export let nav: NavState = {view: 'overview', params: {}};
export let trail: BreadcrumbEntry[] = [makeCrumb(nav)];
export let trailIndex = 0;

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
  trail = [...trail.slice(0, trailIndex + 1), makeCrumb(state)];
  trailIndex = trail.length - 1;
  nav = state;
  navigateCallback?.(stateToSubpage(state));
  m.redraw();
}

export function onBreadcrumbNavigate(i: number): void {
  const crumb = trail[i];
  nav = crumb.state;
  trailIndex = i;
  navigateCallback?.(stateToSubpage(crumb.state));
  m.redraw();
}

export function resetToOverview(): void {
  const state: NavState = {view: 'overview', params: {}};
  trail = [makeCrumb(state)];
  trailIndex = 0;
  nav = state;
}

export function syncFromSubpage(subpage: string | undefined): void {
  if (subpage?.startsWith('/')) subpage = subpage.slice(1);
  // Compare path-only: Perfetto's router strips query params from subpage.
  const currentSubpage = stateToSubpage(nav);
  const currentPath = currentSubpage.split('?')[0];
  const incomingPath = (subpage ?? '').split('?')[0];
  if (incomingPath !== currentPath) {
    const state = subpageToState(subpage);
    nav = state;
    // flamegraph-objects is only reachable via "Open in Heapdump Explorer"
    // from the timeline — preserve the existing trail so the user can
    // navigate back through their previous views.
    if (state.view === 'flamegraph-objects' && trail.length > 1) {
      trail = [...trail.slice(0, trailIndex + 1), makeCrumb(state)];
      trailIndex = trail.length - 1;
    } else {
      trail = [makeCrumb(state)];
      trailIndex = 0;
    }
  }
}

// Shared flag for sidebar sub-item visibility (readable by sidebar callbacks).
export let hasReadyHprofSession = false;
export function setHasReadyHprofSession(v: boolean): void {
  hasReadyHprofSession = v;
}
