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
import HeapProfilePlugin from '../dev.perfetto.HeapProfile';
import type {NavState} from './nav_state';
import type {OverviewData} from './types';
import {Breadcrumbs} from './components';
import {
  nav,
  trail,
  trailIndex,
  navigate,
  onBreadcrumbNavigate,
  syncFromSubpage,
  setNavigateCallback,
} from './nav_state';
import * as queries from './queries';
import OverviewView from './views/overview_view';
import RootedView from './views/rooted_view';
import ObjectView from './views/object_view';
import SearchView from './views/search_view';
import ObjectsView from './views/objects_view';
import BitmapGalleryView from './views/bitmap_gallery_view';
import AllocationsView from './views/allocations_view';
import StringsView from './views/strings_view';
import FlamegraphObjectsView from './views/flamegraph_objects_view';
import {
  consumeFlamegraphAhatSelection,
  type FlamegraphAhatSelection,
} from '../dev.perfetto.HeapProfile/flamegraph_to_ahat';

// ─── View in Timeline ─────────────────────────────────────────────────────────

async function viewInTimeline(objectId: number): Promise<void> {
  const trace = HeapDumpPage.trace;
  if (!trace) return;

  const info = await queries.getHeapGraphTrackInfo(trace.engine, objectId);
  if (!info) return;

  // Set a pending SHOW_FROM_FRAME filter so the flamegraph auto-focuses on
  // the object. Prefer path hash (exact tree node) over class name (all
  // allocations of that class).
  const filter = info.pathHash
    ? `^${info.pathHash}$`
    : info.className
      ? `^${info.className}$`
      : undefined;
  if (filter) {
    const hp = trace.plugins.getPlugin(HeapProfilePlugin);
    hp.setJavaHeapGraphFlamegraphFilter(filter);
  }

  // URI matches HeapProfile plugin's trackUri(upid, 'java_heap_graph').
  const uri = `/process_${info.upid}/java_heap_graph_heap_profile`;
  trace.navigate('#!/viewer');
  trace.selection.selectTrackEvent(uri, info.eventId);
  trace.scrollTo({track: {uri, expandGroup: true}});
}

// Cached flamegraph selection consumed from HeapProfile. Consumed once on first
// render of the flamegraph-objects view, then reused for subsequent renders.
let cachedFlamegraphSelection: FlamegraphAhatSelection | null = null;

/** Reset cached selection on trace change to prevent stale cross-trace data. */
export function resetCachedFlamegraphSelection(): void {
  cachedFlamegraphSelection = null;
}

// Module-level overview cache. Survives component remounts (e.g. theme toggle).
let cachedOverview: OverviewData | null = null;
let overviewLoading = false;

/** Reset cached overview on trace change. */
export function resetCachedOverview(): void {
  cachedOverview = null;
  overviewLoading = false;
}

// ─── Content View Router ──────────────────────────────────────────────────────

function renderContentView(
  state: NavState,
  engine: Engine,
  overview: OverviewData,
): m.Children {
  switch (state.view) {
    case 'overview':
      return m(OverviewView, {overview, name: 'Heap Dump', navigate});
    case 'allocations':
      return m(AllocationsView, {
        engine,
        navigate,
        heaps: overview.heaps,
        params: state.params,
      });
    case 'rooted':
      return m(RootedView, {engine, heaps: overview.heaps, navigate});
    case 'object':
      return m(ObjectView, {
        engine,
        heaps: overview.heaps,
        navigate,
        params: state.params,
        onViewInTimeline: viewInTimeline,
      });
    case 'objects':
      return m(ObjectsView, {engine, navigate, params: state.params});
    case 'search':
      return m(SearchView, {
        engine,
        navigate,
        initialQuery: state.params.q,
      });
    case 'bitmaps':
      return m(BitmapGalleryView, {engine, navigate});
    case 'strings':
      return m(StringsView, {
        engine,
        navigate,
        initialQuery: state.params.q,
      });
    case 'flamegraph-objects': {
      const pending = consumeFlamegraphAhatSelection();
      if (pending) cachedFlamegraphSelection = pending;
      const sel = cachedFlamegraphSelection;
      return m(FlamegraphObjectsView, {
        engine,
        navigate,
        nodeName: state.params.name,
        pathHashes: sel?.pathHashes,
        isDominator: sel?.isDominator,
        onBackToTimeline: () => {
          const trace = HeapDumpPage.trace;
          if (trace) trace.navigate('#!/viewer');
        },
      });
    }
  }
}

// ─── HeapDumpPage ─────────────────────────────────────────────────────────────

interface HeapDumpPageAttrs {
  subpage: string | undefined;
}

export class HeapDumpPage implements m.ClassComponent<HeapDumpPageAttrs> {
  // Set by the plugin's onTraceLoad.
  static engine: Engine | null = null;
  static trace: Trace | null = null;
  static hasHeapData = false;

  oncreate(vnode: m.VnodeDOM<HeapDumpPageAttrs>) {
    setNavigateCallback((subpage) => {
      const href = `#!/ahat${subpage ? '/' + subpage : ''}`;
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
        m('div', {class: 'ah-loading'}, 'No heap graph data in this trace.'),
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
        m(Breadcrumbs, {
          trail,
          activeIndex: trailIndex,
          onNavigate: onBreadcrumbNavigate,
        }),
        renderContentView(nav, HeapDumpPage.engine, cachedOverview),
      ),
    );
  }
}
