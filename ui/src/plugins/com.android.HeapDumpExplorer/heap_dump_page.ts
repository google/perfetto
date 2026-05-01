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
import {Time} from '../../base/time';
import {Spinner} from '../../widgets/spinner';
import {Button, ButtonVariant} from '../../widgets/button';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {Tabs} from '../../widgets/tabs';
import type {TabsTab} from '../../widgets/tabs';
import {formatDuration} from '../../components/time_utils';
import type {NavState, NavView} from './nav_state';
import type {OverviewData} from './types';
import * as queries from './queries';
import OverviewView from './views/overview_view';
import DominatorsView from './views/dominators_view';
import ObjectView from './views/object_view';
import AllObjectsView from './views/all_objects_view';
import BitmapGalleryView from './views/bitmap_gallery_view';
import ClassesView from './views/classes_view';
import StringsView from './views/strings_view';
import ArraysView from './views/arrays_view';
import FlamegraphObjectsView from './views/flamegraph_objects_view';
import type {HeapDumpExplorerSession} from './session';

interface HeapDumpPageAttrs {
  readonly session: HeapDumpExplorerSession;
  readonly subpage: string | undefined;
}

const FG_KEY_PREFIX = 'fg-';
const INSTANCE_KEY_PREFIX = 'inst-';

function fgTabKey(id: number): string {
  return `${FG_KEY_PREFIX}${id}`;
}

function instanceTabKey(id: number): string {
  return `${INSTANCE_KEY_PREFIX}${id}`;
}

function parseFgTabKey(key: string): number | undefined {
  if (!key.startsWith(FG_KEY_PREFIX)) return undefined;
  return parseInt(key.slice(FG_KEY_PREFIX.length), 10);
}

function parseInstanceTabKey(key: string): number | undefined {
  if (!key.startsWith(INSTANCE_KEY_PREFIX)) return undefined;
  return parseInt(key.slice(INSTANCE_KEY_PREFIX.length), 10);
}

function activeTabKey(session: HeapDumpExplorerSession): string {
  const tabs = session.flamegraphTabs;
  if (session.nav.view === 'flamegraph-objects' && tabs.length > 0) {
    const active = tabs.find((t) => t.id === session.activeFlamegraphId);
    return fgTabKey(active ? active.id : tabs[tabs.length - 1].id);
  }
  if (session.activeInstanceId !== null) {
    return instanceTabKey(session.activeInstanceId);
  }
  return session.nav.view;
}

function handleTabChange(session: HeapDumpExplorerSession, key: string): void {
  const fgId = parseFgTabKey(key);
  if (fgId !== undefined) {
    session.setActiveFlamegraphTab(fgId);
    session.navigate('flamegraph-objects');
    return;
  }
  const instId = parseInstanceTabKey(key);
  if (instId !== undefined) {
    session.setActiveInstanceTab(instId);
    const tab = session.instanceTabs.find((t) => t.id === instId);
    if (tab) session.navigate('object', {id: tab.objId});
    return;
  }
  session.clearActiveFlamegraphTab();
  session.clearActiveInstanceTab();
  session.navigate(key as NavView);
}

function handleTabClose(session: HeapDumpExplorerSession, key: string): void {
  const fgId = parseFgTabKey(key);
  if (fgId !== undefined) {
    session.closeFlamegraph(fgId);
    return;
  }
  const instId = parseInstanceTabKey(key);
  if (instId !== undefined) {
    session.closeInstanceTab(instId);
  }
}

function buildTabs(
  session: HeapDumpExplorerSession,
  activeDump: queries.HeapDump,
  state: NavState,
  overview: OverviewData,
): TabsTab[] {
  const {engine, trace, navigateWithTabs, clearNavParam} = session;
  const tabs: TabsTab[] = [
    {
      key: 'overview',
      title: 'Overview',
      content: m(OverviewView, {overview, navigate: navigateWithTabs}),
    },
    {
      key: 'classes',
      title: 'Classes',
      content: m(ClassesView, {
        engine,
        activeDump,
        navigate: navigateWithTabs,
        clearNavParam,
        initialRootClass:
          state.view === 'classes' ? state.params.rootClass : undefined,
      }),
    },
    {
      key: 'objects',
      title: 'Objects',
      content: m(AllObjectsView, {
        engine,
        activeDump,
        navigate: navigateWithTabs,
        clearNavParam,
        initialClass: state.view === 'objects' ? state.params.cls : undefined,
      }),
    },
    {
      key: 'dominators',
      title: 'Dominators',
      content: m(DominatorsView, {
        engine,
        activeDump,
        navigate: navigateWithTabs,
      }),
    },
    {
      key: 'bitmaps',
      title: 'Bitmaps',
      content: m(BitmapGalleryView, {
        engine,
        activeDump,
        navigate: navigateWithTabs,
        clearNavParam,
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
        activeDump,
        navigate: navigateWithTabs,
        clearNavParam,
        initialQuery: state.view === 'strings' ? state.params.q : undefined,
        hasFieldValues: overview.hasFieldValues,
      }),
    },
    {
      key: 'arrays',
      title: 'Arrays',
      content: m(ArraysView, {
        engine,
        activeDump,
        navigate: navigateWithTabs,
        clearNavParam,
        initialArrayHash:
          state.view === 'arrays' ? state.params.arrayHash : undefined,
        hasFieldValues: overview.hasFieldValues,
      }),
    },
  ];

  for (const fg of session.flamegraphTabs) {
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
        onBackToTimeline: () => trace.navigate('#!/viewer'),
      }),
    });
  }

  for (const obj of session.instanceTabs) {
    tabs.push({
      key: instanceTabKey(obj.id),
      title: obj.label,
      closeButton: true,
      content: m(ObjectView, {
        engine,
        activeDump,
        heaps: overview.heaps,
        navigate: navigateWithTabs,
        params: {id: obj.objId},
      }),
    });
  }

  return tabs;
}

function processLabel(d: queries.HeapDump): string {
  return d.processName !== null
    ? `${d.processName} (pid ${d.pid})`
    : `pid ${d.pid}`;
}

function renderDumpSelector(session: HeapDumpExplorerSession): m.Children {
  const allDumps = session.dumps;
  const active = session.activeDump;
  if (allDumps.length <= 1 || active === null) return null;

  return m(
    'div',
    {class: 'ah-dump-selector'},
    m('span', {class: 'ah-dump-selector__label'}, 'Heap dump:'),
    m(
      PopupMenu,
      {
        trigger: m(Button, {
          label: processLabel(active),
          icon: 'memory',
          rightIcon: 'arrow_drop_down',
          variant: ButtonVariant.Outlined,
          compact: true,
        }),
      },
      allDumps.map((d) => {
        const offset = Time.diff(
          Time.fromRaw(d.ts),
          session.trace.traceInfo.start,
        );
        return m(MenuItem, {
          label: `${processLabel(d)} — ${formatDuration(session.trace, offset)}`,
          active: d === active,
          onclick: () => session.selectDump(d),
        });
      }),
    ),
  );
}

export class HeapDumpPage implements m.ClassComponent<HeapDumpPageAttrs> {
  oncreate({attrs}: m.VnodeDOM<HeapDumpPageAttrs>) {
    attrs.session.setNavigateCallback((sub) => {
      window.location.hash = `!/heapdump${sub ? '/' + sub : ''}`;
    });
    void attrs.session.loadOverview();
  }

  onremove({attrs}: m.VnodeDOM<HeapDumpPageAttrs>) {
    attrs.session.setNavigateCallback(undefined);
  }

  view({attrs}: m.Vnode<HeapDumpPageAttrs>) {
    const {session, subpage} = attrs;
    session.syncFromSubpage(subpage);
    session.syncInstanceTabFromNav();

    const active = session.activeDump;
    const overview = session.cachedOverview;
    if (active === null || overview === null) {
      return m(
        'div',
        {class: 'ah-page'},
        renderDumpSelector(session),
        m('div', {class: 'ah-loading'}, m(Spinner, {easing: true})),
      );
    }

    // Keyed so Mithril remounts views (and their SQLDataSources) on
    // dump switch.
    const tabsKey = `${active.upid}:${active.ts}`;

    return m(
      'div',
      {class: 'ah-page'},
      renderDumpSelector(session),
      m(
        'main',
        {class: 'ah-main'},
        m(Tabs, {
          key: tabsKey,
          tabs: buildTabs(session, active, session.nav, overview),
          activeTabKey: activeTabKey(session),
          onTabChange: (key: string) => handleTabChange(session, key),
          onTabClose: (key: string) => handleTabClose(session, key),
        }),
      ),
    );
  }
}
