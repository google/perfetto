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
import DominatorsView from './views/dominators_view';
import ObjectView from './views/object_view';
import AllObjectsView from './views/all_objects_view';
import InstancesView from './views/instances_view';
import BitmapGalleryView from './views/bitmap_gallery_view';
import ClassesView from './views/classes_view';
import StringsView from './views/strings_view';

// Module-level overview cache. Survives component remounts (e.g. theme toggle).
let cachedOverview: OverviewData | null = null;
let overviewLoading = false;

/** Reset cached overview on trace change. */
export function resetCachedOverview(): void {
  cachedOverview = null;
  overviewLoading = false;
}

const PAGE_TABS: ReadonlyArray<{key: string; label: string; view: string}> = [
  {key: 'overview', label: 'Overview', view: 'overview'},
  {key: 'classes', label: 'Classes', view: 'classes'},
  {key: 'objects', label: 'Objects', view: 'objects'},
  {key: 'dominators', label: 'Dominators', view: 'dominators'},
  {key: 'bitmaps', label: 'Bitmaps', view: 'bitmaps'},
  {key: 'strings', label: 'Strings', view: 'strings'},
];

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

function renderContentView(
  state: NavState,
  engine: Engine,
  overview: OverviewData,
): m.Children {
  switch (state.view) {
    case 'overview':
      return m(OverviewView, {
        overview,
        navigate,
      });
    case 'classes':
      return m(ClassesView, {engine, navigate});
    case 'dominators':
      return m(DominatorsView, {engine, navigate});
    case 'objects':
      return m(AllObjectsView, {engine, navigate});
    case 'object':
      return m(ObjectView, {
        engine,
        heaps: overview.heaps,
        navigate,
        params: state.params,
      });
    case 'instances':
      return m(InstancesView, {engine, navigate, params: state.params});
    case 'bitmaps':
      return m(BitmapGalleryView, {
        engine,
        navigate,
        hasFieldValues: overview.hasFieldValues,
        filterKey: state.params.filterKey,
      });
    case 'strings':
      return m(StringsView, {
        engine,
        navigate,
        initialQuery: state.params.q,
        hasFieldValues: overview.hasFieldValues,
      });
    default:
      return null;
  }
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

    const currentTab = activeTabKey(nav.view);
    const showBreadcrumbs = nav.view === 'object' || nav.view === 'instances';
    const isScrollView =
      nav.view === 'object' ||
      nav.view === 'overview' ||
      nav.view === 'bitmaps';

    return m(
      'div',
      {class: 'ah-page' + (isScrollView ? ' ah-page--scroll' : '')},
      m(
        'main',
        {class: 'ah-main'},
        m('div', {class: 'ah-tab-bar'}, [
          PAGE_TABS.map((tab) =>
            m(
              'button',
              {
                key: tab.key,
                class:
                  'ah-tab-btn' +
                  (currentTab === tab.key ? ' ah-tab-btn--active' : ''),
                onclick: () => navigate(tab.view as NavState['view']),
              },
              tab.label,
            ),
          ),
        ]),
        showBreadcrumbs
          ? m(Breadcrumbs, {
              trail,
              activeIndex: trailIndex,
              onNavigate: onBreadcrumbNavigate,
            })
          : null,
        renderContentView(nav, HeapDumpPage.engine, cachedOverview),
      ),
    );
  }
}
