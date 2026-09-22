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
import {Anchor} from '../../widgets/anchor';
import {EmptyState} from '../../widgets/empty_state';
import {formatDuration} from '../../components/time_utils';
import {stateToSubpage} from './nav_state';
import type {NavView} from './nav_state';
import type * as queries from './queries';
import {OverviewView} from './views/overview_view';
import {DominatorsView} from './views/dominators_view';
import {FlamegraphView} from './views/flamegraph_view';
import {ClassesView} from './views/classes_view';
import {AllObjectsView} from './views/all_objects_view';
import {BitmapGalleryView} from './views/bitmap_gallery_view';
import {StringsView} from './views/strings_view';
import {ArraysView} from './views/arrays_view';
import {CallstackView} from './views/callstack_view';
import {FlamegraphObjectsView} from './views/flamegraph_objects_view';
import {ObjectView} from './views/object_view';
import type {LazyTab} from './views/lazy_tabs';
import {LazyTabs} from './views/lazy_tabs';
import type {HeapDumpExplorerSession} from './session';
import {type Routes, Router} from './router';

interface HeapDumpPageAttrs {
  readonly session: HeapDumpExplorerSession;
  readonly subpage: string | undefined;
}

const FG_KEY_PREFIX = 'fg-';
const INSTANCE_KEY_PREFIX = 'inst-';

function fgTabKey(pathHashes: string, isDominator: boolean): string {
  return `${FG_KEY_PREFIX}${isDominator ? 'd' : 'n'}:${pathHashes}`;
}

function instanceTabKey(objId: number): string {
  return `${INSTANCE_KEY_PREFIX}${objId}`;
}

// Per-tab select/close actions, looked up by key.
interface TabActions {
  select(): void;
  close?(): void;
}

// Builds the tab handles (titles only — page content is rendered lazily by the
// sub-router, one route per tab) plus the per-tab select/close actions.
function buildTabs(session: HeapDumpExplorerSession): {
  handles: LazyTab[];
  actions: Map<string, TabActions>;
} {
  const actions = new Map<string, TabActions>();

  const handles: LazyTab[] = [
    {key: 'overview', title: 'Overview', href: '#!/heapdump/overview'},
    {key: 'flamegraph', title: 'Flamegraph', href: '#!/heapdump/flamegraph'},
    {key: 'classes', title: 'Classes', href: '#!/heapdump/classes'},
    {key: 'objects', title: 'Objects', href: '#!/heapdump/objects'},
    {key: 'dominators', title: 'Dominators', href: '#!/heapdump/dominators'},
    {key: 'bitmaps', title: 'Bitmaps', href: '#!/heapdump/bitmaps'},
    {key: 'strings', title: 'Strings', href: '#!/heapdump/strings'},
    {key: 'arrays', title: 'Arrays', href: '#!/heapdump/arrays'},
    {key: 'callstack', title: 'Callstack', href: '#!/heapdump/callstack'},
  ];

  for (const fg of session.flamegraphTabs) {
    const key = fgTabKey(fg.pathHashes, fg.isDominator);
    const title =
      fg.count !== null
        ? `Flamegraph objects (${fg.count.toLocaleString()})`
        : 'Flamegraph objects';
    handles.push({
      key,
      title,
      href: `#!/heapdump/${stateToSubpage({
        view: 'flamegraph-objects',
        params: {pathHashes: fg.pathHashes, isDominator: fg.isDominator},
      })}`,
    });
    actions.set(key, {
      select: () =>
        session.navigate('flamegraph-objects', {
          pathHashes: fg.pathHashes,
          isDominator: fg.isDominator,
        }),
      close: () => session.closeFlamegraph(fg.pathHashes, fg.isDominator),
    });
  }

  for (const obj of session.instanceTabs) {
    const key = instanceTabKey(obj.objId);
    handles.push({
      key,
      title: obj.label,
      href: `#!/heapdump/${stateToSubpage({view: 'object', params: {id: obj.objId}})}`,
    });
    actions.set(key, {
      select: () => session.navigate('object', {id: obj.objId}),
      close: () => session.closeInstanceTab(obj.objId),
    });
  }

  return {handles, actions};
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
    {class: 'pf-hde-dump-selector'},
    m('span', {class: 'pf-hde-dump-selector__label'}, 'Heap dump:'),
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
        const offset = Time.diff(d.ts, session.trace.traceInfo.start);
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
    session.syncFlamegraphTabFromNav();

    const active = session.activeDump;
    const overview = session.cachedOverview;
    if (active === null || overview === null) {
      return m(
        'div',
        {class: 'pf-hde-page'},
        renderDumpSelector(session),
        m('div', {class: 'pf-hde-loading'}, m(Spinner, {easing: true})),
      );
    }

    const {handles, actions} = buildTabs(session);
    const {engine, trace, navigateWithTabs, clearNavParam} = session;

    // Renders the page chrome (dump selector + tab bar) around the content of
    // the currently active route. LazyTabs keeps the DOM of every visited tab
    // alive (gated off-screen) so switching tabs doesn't remount their data
    // sources.
    const renderPage = (
      content: m.Children,
      // The key of the tab to highlight, computed by the route; pass '' to
      // render the tab bar with no tab highlighted (e.g. the not-found page).
      activeKey: string,
    ): m.Children => {
      // Keyed so Mithril remounts the tab pages (and their SQLDataSources) on
      // dump switch.
      const tabsKey = `${active.upid}:${active.ts}`;
      return m(
        'div',
        {class: 'pf-hde-page'},
        renderDumpSelector(session),
        m(
          'main',
          {class: 'pf-hde-page__tabs'},
          m(LazyTabs, {
            key: tabsKey,
            handles: handles.map((handle) => ({
              ...handle,
              onclick: () => actions.get(handle.key)?.select(),
            })),
            contentForKey: activeKey,
            fillHeight: true,
            page: content,
          }),
        ),
      );
    };

    // Vnodes are created inside the route handlers (not up front) so a route
    // only pays for its page when it is actually matched.
    const renderOverview = () =>
      renderPage(
        m(OverviewView, {
          overview,
          activeDump: active,
          navigate: navigateWithTabs,
          showDefaultChangedHint:
            session.autoNavigated && !session.hideDefaultChangedHint.get(),
          onBackToTimeline: () => trace.navigate('#!/viewer'),
          onDismissDefaultChangedHint: () =>
            session.hideDefaultChangedHint.set(true),
        }),
        'overview',
      );

    // `cls` comes from the path (objects/<class>); the query-less route
    // renders the unfiltered grid.
    const renderObjects = (cls?: string) =>
      renderPage(
        m(AllObjectsView, {
          engine,
          activeDump: active,
          navigate: navigateWithTabs,
          initialClass: cls,
        }),
        'objects',
      );

    // TODO: plumb the bitmap id through once BitmapGalleryView supports
    // opening at a specific bitmap.
    const renderBitmaps = () =>
      renderPage(
        m(BitmapGalleryView, {
          engine,
          activeDump: active,
          navigate: navigateWithTabs,
          clearNavParam,
          hasFieldValues: overview.hasFieldValues,
        }),
        'bitmaps',
      );

    const routes: Routes = {
      '': renderOverview,
      'overview': renderOverview,
      'flamegraph': () =>
        renderPage(
          m(FlamegraphView, {
            trace,
            upid: active.upid,
            ts: active.ts,
            state: session.flamegraphPanelState,
            onStateChange: session.setFlamegraphPanelState,
            onShowObjects: (pathHashes, isDominator) =>
              session.openFlamegraph({
                pathHashes,
                isDominator,
                upid: active.upid,
                ts: active.ts,
              }),
          }),
          'flamegraph',
        ),
      'dominators': () =>
        renderPage(
          m(DominatorsView, {
            engine,
            activeDump: active,
            navigate: navigateWithTabs,
          }),
          'dominators',
        ),
      'classes': () =>
        renderPage(
          m(ClassesView, {
            engine,
            activeDump: active,
            navigate: navigateWithTabs,
            clearNavParam,
          }),
          'classes',
        ),
      'objects': () => renderObjects(),
      'objects/:cls': ({params: {cls}}) => renderObjects(cls),
      'object/:id': ({params: {id}}) =>
        renderPage(
          m(ObjectView, {
            engine,
            activeDump: active,
            heaps: overview.heaps,
            navigate: navigateWithTabs,
            openFlamegraphPivotedAt: session.openFlamegraphPivotedAt,
            // The router stores ids hex-encoded (object/0x1a2b).
            params: {id: Number.parseInt(id, 16)},
          }),
          instanceTabKey(Number.parseInt(id, 16)),
        ),
      'bitmaps': renderBitmaps,
      'bitmaps/:id': renderBitmaps,
      'strings': () =>
        renderPage(
          m(StringsView, {
            engine,
            activeDump: active,
            navigate: navigateWithTabs,
            clearNavParam,
            hasFieldValues: overview.hasFieldValues,
          }),
          'strings',
        ),
      'arrays': () =>
        renderPage(
          m(ArraysView, {
            engine,
            activeDump: active,
            navigate: navigateWithTabs,
            clearNavParam,
            hasFieldValues: overview.hasFieldValues,
          }),
          'arrays',
        ),
      'callstack': () =>
        renderPage(
          m(CallstackView, {
            trace,
            dump: active,
            state: session.callstackPanelState,
            onStateChange: session.setCallstackPanelState,
          }),
          'callstack',
        ),
      'flamegraph_objects/:dom/:pathHashes': ({params: {dom, pathHashes}}) =>
        renderPage(
          m(FlamegraphObjectsView, {
            engine,
            navigate: navigateWithTabs,
            pathHashes,
            isDominator: dom === '1',
            onBackToTimeline: () => trace.navigate('#!/viewer'),
          }),
          fgTabKey(pathHashes, dom === '1'),
        ),
    };

    return m(Router, {
      path: subpage,
      routes,
      fallback: () =>
        renderPage(
          m(
            EmptyState,
            {
              title: 'Page not found',
              fillHeight: true,
            },
            m(Anchor, {href: '#!/heapdump'}, 'Back to overview'),
          ),
          '',
        ),
    });
  }
}
