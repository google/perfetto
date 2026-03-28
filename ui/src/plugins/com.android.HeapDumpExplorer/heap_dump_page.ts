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

import m from 'mithril';
import type {Engine} from '../../trace_processor/engine';
import type {Trace} from '../../public/trace';
import {Spinner} from '../../widgets/spinner';
import {EmptyState} from '../../widgets/empty_state';
import {Tabs} from '../../widgets/tabs';
import type {TabsTab} from '../../widgets/tabs';
interface HeapdumpSelection {
  pathHashes: string;
  isDominator: boolean;
}
import type {NavState} from './nav_state';
import type {OverviewData} from './types';
import {nav, navigate, syncFromSubpage, setNavigateCallback} from './nav_state';
import * as queries from './queries';
import OverviewView from './views/overview_view';
import DominatorsView from './views/dominators_view';
import ObjectView from './views/object_view';
import AllObjectsView from './views/all_objects_view';
import BitmapGalleryView from './views/bitmap_gallery_view';
import ClassesView from './views/classes_view';
import StringsView from './views/strings_view';
import ArraysView from './views/arrays_view';
import FlamegraphObjectsView, {
  flamegraphQuery,
} from './views/flamegraph_objects_view';
import {SQL_PREAMBLE} from './components';
import {NUM} from '../../trace_processor/query_result';

// Each "Open in Heapdump Explorer" creates a closable flamegraph tab.
let nextFgId = 0;
const flamegraphTabs: Array<
  {id: number; count: number | null} & HeapdumpSelection
> = [];
let activeFgId = -1;

export function setFlamegraphSelection(
  sel: HeapdumpSelection,
  engine: Engine,
): void {
  const existing = flamegraphTabs.find(
    (t) => t.pathHashes === sel.pathHashes && t.isDominator === sel.isDominator,
  );
  if (existing) {
    activeFgId = existing.id;
    navigate('flamegraph-objects');
    return;
  }
  const id = nextFgId++;
  const tab = {id, count: null as number | null, ...sel};
  flamegraphTabs.push(tab);
  activeFgId = id;
  const q = flamegraphQuery(sel.pathHashes, sel.isDominator);
  engine
    .query(`${SQL_PREAMBLE}; SELECT COUNT(*) AS c FROM (${q})`)
    .then((r) => {
      tab.count = Number(r.firstRow({c: NUM}).c);
      m.redraw();
    });
}

export function resetFlamegraphSelection(): void {
  flamegraphTabs.length = 0;
  nextFgId = 0;
  activeFgId = -1;
}

// Module-level overview cache. Survives component remounts (e.g. theme toggle).
let cachedOverview: OverviewData | null = null;
let overviewLoading = false;

/** Reset cached overview on trace change. */
export function resetCachedOverview(): void {
  cachedOverview = null;
  overviewLoading = false;
}

// Closable object tabs — clicking an object anywhere opens a new tab.
interface InstanceTab {
  id: number;
  objId: number;
  label: string;
}

let nextInstanceTabId = 0;
const instanceTabs: InstanceTab[] = [];
let activeInstanceTabId = -1;

function instanceTabKey(id: number): string {
  return `inst-${id}`;
}

export function resetInstanceTabs(): void {
  instanceTabs.length = 0;
  nextInstanceTabId = 0;
  activeInstanceTabId = -1;
}

function openInstanceTab(objId: number, label?: string): void {
  const existing = instanceTabs.find((t) => t.objId === objId);
  if (existing) {
    activeInstanceTabId = existing.id;
    return;
  }
  const displayLabel = label ?? 'Instance';
  const tab: InstanceTab = {
    id: nextInstanceTabId++,
    objId,
    label:
      displayLabel.length > 30
        ? displayLabel.slice(0, 30) + '\u2026'
        : displayLabel,
  };
  instanceTabs.push(tab);
  activeInstanceTabId = tab.id;
}

// Navigate wrapper: intercepts 'object' to open closable instance tabs.
function navigateWithTabs(
  view: NavState['view'],
  params?: Record<string, unknown>,
): void {
  if (view === 'object') {
    openInstanceTab(params?.id as number, params?.label as string | undefined);
    navigate(view, params);
    return;
  }
  activeInstanceTabId = -1;
  navigate(view, params);
}

// When nav state points to 'object' (e.g. after browser back), ensure
// the matching instance tab exists and is active. When nav moves away
// from 'object', clear the active instance tab so fixed tabs are shown.
function syncInstanceTabFromNav(): void {
  if (nav.view !== 'object') {
    activeInstanceTabId = -1;
    return;
  }
  const objId = nav.params.id;
  const existing = instanceTabs.find((t) => t.objId === objId);
  if (existing) {
    activeInstanceTabId = existing.id;
  } else {
    openInstanceTab(objId, nav.params.label);
  }
}

function fgTabKey(id: number): string {
  return `fg-${id}`;
}

function parseFgTabKey(key: string): number | undefined {
  if (!key.startsWith('fg-')) return undefined;
  return parseInt(key.slice(3), 10);
}

function getActiveTabKey(): string {
  if (nav.view === 'flamegraph-objects' && flamegraphTabs.length > 0) {
    const tab = flamegraphTabs.find((t) => t.id === activeFgId);
    return fgTabKey(
      tab ? tab.id : flamegraphTabs[flamegraphTabs.length - 1].id,
    );
  }
  if (activeInstanceTabId >= 0) {
    return instanceTabKey(activeInstanceTabId);
  }
  return nav.view;
}

function handleTabChange(key: string): void {
  const fgId = parseFgTabKey(key);
  if (fgId !== undefined) {
    activeFgId = fgId;
    navigate('flamegraph-objects');
  } else if (key.startsWith('inst-')) {
    activeInstanceTabId = parseInt(key.slice(5), 10);
    const tab = instanceTabs.find((t) => t.id === activeInstanceTabId);
    if (tab) {
      navigate('object', {id: tab.objId});
    }
  } else {
    activeFgId = -1;
    activeInstanceTabId = -1;
    navigate(key as NavState['view']);
  }
}

function handleTabClose(key: string): void {
  const fgId = parseFgTabKey(key);
  if (fgId !== undefined) {
    const idx = flamegraphTabs.findIndex((t) => t.id === fgId);
    if (idx === -1) return;
    flamegraphTabs.splice(idx, 1);
    if (activeFgId === fgId) {
      activeFgId = -1;
      navigate('overview');
    }
    return;
  }
  if (!key.startsWith('inst-')) return;
  const id = parseInt(key.slice(5), 10);
  const idx = instanceTabs.findIndex((t) => t.id === id);
  if (idx === -1) return;
  instanceTabs.splice(idx, 1);
  if (activeInstanceTabId === id) {
    activeInstanceTabId = -1;
    navigate('overview');
  }
}

function buildTabs(
  state: NavState,
  engine: Engine,
  overview: OverviewData,
): TabsTab[] {
  const trace = HeapDumpPage.trace;
  const tabs: TabsTab[] = [
    {
      key: 'overview',
      title: 'Overview',
      content: m(OverviewView, {overview, navigate: navigateWithTabs}),
    },
    {
      key: 'classes',
      title: 'Classes',
      content: m(ClassesView, {engine, navigate: navigateWithTabs}),
    },
    {
      key: 'objects',
      title: 'Objects',
      content: m(AllObjectsView, {
        engine,
        navigate: navigateWithTabs,
        initialClass: state.view === 'objects' ? state.params.cls : undefined,
      }),
    },
    {
      key: 'dominators',
      title: 'Dominators',
      content: m(DominatorsView, {engine, navigate: navigateWithTabs}),
    },
    {
      key: 'bitmaps',
      title: 'Bitmaps',
      content: m(BitmapGalleryView, {
        engine,
        navigate: navigateWithTabs,
        hasFieldValues: overview.hasFieldValues,
        filterKey:
          state.view === 'bitmaps' ? state.params.filterKey : undefined,
      }),
    },
    {
      key: 'strings',
      title: 'Strings',
      content: m(StringsView, {
        engine,
        navigate: navigateWithTabs,
        initialQuery: state.view === 'strings' ? state.params.q : undefined,
        hasFieldValues: overview.hasFieldValues,
      }),
    },
    {
      key: 'arrays',
      title: 'Arrays',
      content: m(ArraysView, {
        engine,
        navigate: navigateWithTabs,
        initialArrayHash:
          state.view === 'arrays' ? state.params.arrayHash : undefined,
        hasFieldValues: overview.hasFieldValues,
      }),
    },
  ];

  // Append closable flamegraph tabs.
  for (const fg of flamegraphTabs) {
    tabs.push({
      key: fgTabKey(fg.id),
      title:
        fg.count !== null
          ? `Flamegraph (${fg.count.toLocaleString()})`
          : 'Flamegraph',
      closeButton: true,
      content: m(FlamegraphObjectsView, {
        engine,
        navigate: navigateWithTabs,
        pathHashes: fg.pathHashes,
        isDominator: fg.isDominator,
        onBackToTimeline: () => {
          if (trace) trace.navigate('#!/viewer');
        },
      }),
    });
  }

  // Append closable object instance tabs.
  for (const obj of instanceTabs) {
    tabs.push({
      key: instanceTabKey(obj.id),
      title: obj.label,
      closeButton: true,
      content: m(ObjectView, {
        engine,
        heaps: overview.heaps,
        navigate: navigateWithTabs,
        params: {id: obj.objId},
      }),
    });
  }

  return tabs;
}

interface HeapDumpPageAttrs {
  readonly subpage: string | undefined;
}

export class HeapDumpPage implements m.ClassComponent<HeapDumpPageAttrs> {
  static engine: Engine | null = null;
  static trace: Trace | null = null;
  static hasHeapData = false;

  oncreate(vnode: m.VnodeDOM<HeapDumpPageAttrs>) {
    setNavigateCallback((subpage) => {
      const href = `#!/heapdump${subpage ? '/' + subpage : ''}`;
      window.location.hash = href.slice(1);
    });
    syncFromSubpage(vnode.attrs.subpage);
    this.loadOverview();
  }

  onremove() {
    setNavigateCallback(undefined);
  }

  private async loadOverview() {
    if (!HeapDumpPage.engine || overviewLoading || cachedOverview) return;
    overviewLoading = true;
    try {
      cachedOverview = await queries.getOverview(HeapDumpPage.engine);
    } catch (err) {
      console.error('Failed to load overview:', err);
    } finally {
      overviewLoading = false;
      m.redraw();
    }
  }

  view(vnode: m.Vnode<HeapDumpPageAttrs>) {
    syncFromSubpage(vnode.attrs.subpage);
    syncInstanceTabFromNav();

    if (!HeapDumpPage.engine || !HeapDumpPage.hasHeapData) {
      return m(
        'div',
        {class: 'ah-page'},
        m(EmptyState, {
          icon: 'memory',
          title: 'No heap graph data in this trace',
          fillHeight: true,
        }),
      );
    }

    if (!cachedOverview) {
      return m(
        'div',
        {class: 'ah-page'},
        m('div', {class: 'ah-loading'}, m(Spinner, {easing: true})),
      );
    }

    return m(
      'div',
      {class: 'ah-page'},
      m(
        'main',
        {class: 'ah-main'},
        m(Tabs, {
          tabs: buildTabs(nav, HeapDumpPage.engine, cachedOverview),
          activeTabKey: getActiveTabKey(),
          onTabChange: handleTabChange,
          onTabClose: handleTabClose,
        }),
      ),
    );
  }
}
