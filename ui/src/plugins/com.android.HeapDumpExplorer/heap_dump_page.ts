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
import InstancesView from './views/instances_view';
import BitmapGalleryView from './views/bitmap_gallery_view';
import ClassesView from './views/classes_view';
import StringsView from './views/strings_view';
import ArraysView from './views/arrays_view';
import FlamegraphObjectsView from './views/flamegraph_objects_view';

// Each "Open in Heapdump Explorer" creates a closable flamegraph tab.
let nextFgId = 0;
const flamegraphTabs: Array<{id: number} & HeapdumpSelection> = [];
let activeFgId = -1;

export function setFlamegraphSelection(sel: HeapdumpSelection): void {
  const id = nextFgId++;
  flamegraphTabs.push({id, ...sel});
  activeFgId = id;
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

// Maps drill-down views to their parent tab.
function activeTabKey(view: string): string {
  switch (view) {
    case 'object':
    case 'instances':
      return 'classes';
    default:
      return view;
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
  return activeTabKey(nav.view);
}

function handleTabChange(key: string): void {
  const id = parseFgTabKey(key);
  if (id !== undefined) {
    activeFgId = id;
    navigate('flamegraph-objects');
  } else {
    activeFgId = -1;
    navigate(key as NavState['view']);
  }
}

function handleTabClose(key: string): void {
  const id = parseFgTabKey(key);
  if (id === undefined) return;
  const idx = flamegraphTabs.findIndex((t) => t.id === id);
  if (idx === -1) return;
  flamegraphTabs.splice(idx, 1);
  if (activeFgId === id) {
    activeFgId = -1;
    navigate('overview');
  }
}

// Renders the content for the "classes" tab, which also hosts drill-down
// views (instances, object detail) based on the current nav state.
function classesTabContent(
  state: NavState,
  engine: Engine,
  overview: OverviewData,
): m.Children {
  switch (state.view) {
    case 'object':
      return m(ObjectView, {
        engine,
        heaps: overview.heaps,
        navigate,
        params: state.params,
      });
    case 'instances':
      return m(InstancesView, {engine, navigate, params: state.params});
    default:
      return m(ClassesView, {engine, navigate});
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
      content: m(OverviewView, {overview, navigate}),
    },
    {
      key: 'classes',
      title: 'Classes',
      content: classesTabContent(state, engine, overview),
    },
    {
      key: 'objects',
      title: 'Objects',
      content: m(AllObjectsView, {engine, navigate}),
    },
    {
      key: 'dominators',
      title: 'Dominators',
      content: m(DominatorsView, {engine, navigate}),
    },
    {
      key: 'bitmaps',
      title: 'Bitmaps',
      content: m(BitmapGalleryView, {
        engine,
        navigate,
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
        navigate,
        initialQuery: state.view === 'strings' ? state.params.q : undefined,
        hasFieldValues: overview.hasFieldValues,
      }),
    },
    {
      key: 'arrays',
      title: 'Arrays',
      content: m(ArraysView, {
        key: state.view === 'arrays' ? state.params.arrayHash ?? '' : '',
        engine,
        navigate,
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
      title: 'Flamegraph',
      closeButton: true,
      content: m(FlamegraphObjectsView, {
        engine,
        navigate,
        pathHashes: fg.pathHashes,
        isDominator: fg.isDominator,
        onBackToTimeline: () => {
          if (trace) trace.navigate('#!/viewer');
        },
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
