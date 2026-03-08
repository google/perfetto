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

// Internal navigation state for the Ahat plugin.
// Uses Perfetto's routing via subpage strings rather than window.history.

import m from 'mithril';

export type NavState =
  | {view: 'overview'; params: Record<string, never>}
  | {view: 'allocations'; params: {heap: string | null}}
  | {view: 'rooted'; params: Record<string, never>}
  | {view: 'object'; params: {id: number; label?: string}}
  | {
      view: 'objects';
      params: {className: string; heap: string | null};
    }
  | {view: 'search'; params: {q: string}}
  | {view: 'bitmaps'; params: {id?: number}}
  | {view: 'strings'; params: {q?: string}}
  | {view: 'flamegraph-objects'; params: {name?: string}};

export function navLabel(state: NavState): string {
  switch (state.view) {
    case 'overview':
      return 'Overview';
    case 'allocations':
      return 'Allocations';
    case 'rooted':
      return 'Rooted';
    case 'object':
      return state.params.label ?? `Object 0x${state.params.id.toString(16)}`;
    case 'objects': {
      const cls = state.params.className;
      const short = cls.includes('.')
        ? cls.slice(cls.lastIndexOf('.') + 1)
        : cls;
      return short || 'Objects';
    }
    case 'search':
      return 'Search';
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

// Subpage string encoding/decoding for Perfetto routing.

export function stateToSubpage(state: NavState): string {
  switch (state.view) {
    case 'overview':
      return '';
    case 'allocations': {
      const h = state.params.heap;
      return h ? `allocations?heap=${encodeURIComponent(h)}` : 'allocations';
    }
    case 'rooted':
      return 'rooted';
    case 'object':
      return `object/0x${state.params.id.toString(16)}`;
    case 'objects': {
      const sp = new URLSearchParams();
      sp.set('class', state.params.className);
      if (state.params.heap) sp.set('heap', state.params.heap);
      return `objects?${sp.toString()}`;
    }
    case 'search': {
      const q = state.params.q;
      return q ? `search?q=${encodeURIComponent(q)}` : 'search';
    }
    case 'bitmaps': {
      const id = state.params.id;
      return id !== undefined ? `bitmaps/0x${id.toString(16)}` : 'bitmaps';
    }
    case 'strings': {
      const q = state.params.q;
      return q ? `strings?q=${encodeURIComponent(q)}` : 'strings';
    }
    case 'flamegraph-objects': {
      const n = state.params.name;
      return n
        ? `flamegraph-objects/${encodeURIComponent(n)}`
        : 'flamegraph-objects';
    }
  }
}

export function subpageToState(subpage: string | undefined): NavState {
  if (!subpage) return {view: 'overview', params: {}};

  const [path, queryStr] = subpage.split('?', 2);
  const sp = new URLSearchParams(queryStr ?? '');
  const parts = path.split('/');
  const view = parts[0];

  switch (view) {
    case '':
    case 'overview':
      return {view: 'overview', params: {}};
    case 'allocations': {
      const heap = sp.get('heap') || null;
      return {view: 'allocations', params: {heap}};
    }
    case 'rooted':
      return {view: 'rooted', params: {}};
    case 'object': {
      const raw = parts[1] ?? '0';
      const id = raw.startsWith('0x')
        ? parseInt(raw.slice(2), 16)
        : parseInt(raw, 10);
      return {view: 'object', params: {id: id || 0}};
    }
    case 'objects': {
      const className = sp.get('class') ?? '';
      const heap = sp.get('heap') || null;
      return {view: 'objects', params: {className, heap}};
    }
    case 'search': {
      const q = sp.get('q') ?? '';
      return {view: 'search', params: {q}};
    }
    case 'bitmaps': {
      const raw = parts[1] ?? '';
      const selectedId = raw.startsWith('0x')
        ? parseInt(raw.slice(2), 16)
        : raw
          ? parseInt(raw, 10)
          : 0;
      return {
        view: 'bitmaps',
        params: selectedId ? {id: selectedId} : {},
      };
    }
    case 'strings': {
      const q = sp.get('q') ?? '';
      return {view: 'strings', params: q ? {q} : {}};
    }
    case 'flamegraph-objects': {
      const n = parts[1] ? decodeURIComponent(parts[1]) : undefined;
      return {view: 'flamegraph-objects', params: n ? {name: n} : {}};
    }
    default:
      return {view: 'overview', params: {}};
  }
}

// Mutable navigation state — same pattern as ahat-web-main's navigation.ts
// but driven by subpage strings instead of window.history.

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
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  p: Record<string, any> = {},
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
  // Perfetto passes subpage with a leading '/' (e.g. '/rooted') — strip it.
  if (subpage?.startsWith('/')) subpage = subpage.slice(1);
  // Perfetto's router (Router.parseFragment) uses new URL(hash) which strips
  // query parameters from the subpage into route.args. So we only get the path
  // portion here. Compare path-only to avoid resetting the trail on every
  // redraw when the current state has query params (e.g. objects?class=...).
  const currentSubpage = stateToSubpage(nav);
  const currentPath = currentSubpage.split('?')[0];
  const incomingPath = (subpage ?? '').split('?')[0];
  if (incomingPath !== currentPath) {
    const state = subpageToState(subpage);
    nav = state;
    // flamegraph-objects is only reachable via "Open in Ahat" from the
    // timeline — preserve the existing trail so the user can navigate back
    // through their previous Ahat views.
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
