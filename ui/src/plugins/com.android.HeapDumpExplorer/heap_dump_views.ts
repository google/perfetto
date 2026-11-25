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
import {formatDuration} from '../../components/time_utils';
import {Button, ButtonVariant} from '../../widgets/button';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import type {time} from '../../base/time';
import type {Setting} from '../../public/settings';
import type {Trace} from '../../public/trace';
import type {Engine} from '../../trace_processor/engine';
import type {TabsTab} from '../../widgets/tabs';
import type {TreeExplorerState} from '../../widgets/tree_explorer';
import type {NavState, NavView} from './nav_state';
import type {FlamegraphTabView} from './session';
import type {HeapDump} from './types';
import {OverviewView} from './views/overview_view';
import {DominatorsView} from './views/dominators_view';
import {ObjectView} from './views/object_view';
import {AllObjectsView} from './views/all_objects_view';
import {BitmapGalleryView} from './views/bitmap_gallery_view';
import {ClassesView} from './views/classes_view';
import {StringsView} from './views/strings_view';
import {ArraysView} from './views/arrays_view';
import {FlamegraphObjectsView} from './views/flamegraph_objects_view';
import {FlamegraphView} from './views/flamegraph_view';
import {CallstackView} from './views/callstack_view';

// What the heap dump views need from their owner. HeapDumpExplorerSession
// implements it; pages hosting the views pass the host through.
export interface HeapDumpViewHost {
  readonly trace: Trace;
  readonly engine: Engine;
  readonly nav: NavState;
  readonly hasFieldValues: boolean;
  readonly autoNavigated: boolean;
  readonly hideDefaultChangedHint: Setting<boolean>;
  readonly flamegraphTabs: ReadonlyArray<FlamegraphTabView>;
  readonly activeFlamegraph: {pathHashes: string; isDominator: boolean} | null;
  readonly instanceTabs: ReadonlyArray<{objId: number; label: string}>;
  readonly activeInstanceObjId: number | null;
  readonly flamegraphPanelState: TreeExplorerState | undefined;
  readonly callstackPanelState: TreeExplorerState | undefined;
  navigate(view: NavView, params?: Record<string, unknown>): void;
  navigateWithTabs(view: NavView, params?: Record<string, unknown>): void;
  clearNavParam(key: string): void;
  setFlamegraphPanelState(state: TreeExplorerState): void;
  setCallstackPanelState(state: TreeExplorerState): void;
  openFlamegraph(sel: {
    pathHashes: string;
    isDominator: boolean;
    upid: number;
    ts: time;
  }): void;
  closeFlamegraph(pathHashes: string, isDominator: boolean): void;
  closeInstanceTab(objId: number): void;
  openFlamegraphPivotedAt(
    pathHash: string,
    label: string,
    isDominator: boolean,
  ): void;
}

const FG_KEY_PREFIX = 'fg-';
const INSTANCE_KEY_PREFIX = 'inst-';

function fgTabKey(pathHashes: string, isDominator: boolean): string {
  return `${FG_KEY_PREFIX}${isDominator ? 'd' : 'n'}:${pathHashes}`;
}

function instanceTabKey(objId: number): string {
  return `${INSTANCE_KEY_PREFIX}${objId}`;
}

export function heapDumpActiveTabKey(host: HeapDumpViewHost): string {
  const tabs = host.flamegraphTabs;
  if (host.nav.view === 'flamegraph-objects' && tabs.length > 0) {
    const active = host.activeFlamegraph;
    const tab =
      (active &&
        tabs.find(
          (t) =>
            t.pathHashes === active.pathHashes &&
            t.isDominator === active.isDominator,
        )) ||
      tabs[tabs.length - 1]!;
    return fgTabKey(tab.pathHashes, tab.isDominator);
  }
  const objId = host.activeInstanceObjId;
  if (objId !== null) {
    return instanceTabKey(objId);
  }
  return host.nav.view;
}

// Per-tab select/close actions, looked up by key.
export interface TabActions {
  select(): void;
  close?(): void;
}

export function buildHeapDumpTabs(
  host: HeapDumpViewHost,
  activeDump: HeapDump,
  state: NavState,
  potentiallyMergedClasses: ReadonlySet<string>,
  overviewTitle = 'Overview',
): {tabs: TabsTab[]; actions: Map<string, TabActions>} {
  const {engine, trace} = host;
  const navigateWithTabs = host.navigateWithTabs.bind(host);
  const clearNavParam = host.clearNavParam.bind(host);
  const hideExplanationSetting = host.hideDefaultChangedHint;
  const hideHint = hideExplanationSetting.get();
  const actions = new Map<string, TabActions>();

  const tabs: TabsTab[] = [
    {
      key: 'overview',
      title: overviewTitle,
      content: m(OverviewView, {
        engine,
        activeDump,
        hasFieldValues: host.hasFieldValues,
        navigate: navigateWithTabs,
        showDefaultChangedHint: host.autoNavigated && !hideHint,
        onBackToTimeline: () => trace.navigate('#!/viewer'),
        onDismissDefaultChangedHint: () => hideExplanationSetting.set(true),
      }),
    },
    {
      key: 'flamegraph',
      title: 'Flamegraph',
      content: m(FlamegraphView, {
        trace,
        upid: activeDump.upid,
        ts: activeDump.ts,
        state: host.flamegraphPanelState,
        onStateChange: host.setFlamegraphPanelState.bind(host),
        onShowObjects: (pathHashes, isDominator) =>
          host.openFlamegraph({
            pathHashes,
            isDominator,
            upid: activeDump.upid,
            ts: activeDump.ts,
          }),
      }),
    },
    {
      key: 'classes',
      title: 'Classes',
      content: m(ClassesView, {
        engine,
        activeDump,
        navigate: navigateWithTabs,
        clearNavParam,
        potentiallyMergedClasses,
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
        potentiallyMergedClasses,
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
        potentiallyMergedClasses,
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
        hasFieldValues: host.hasFieldValues,
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
        hasFieldValues: host.hasFieldValues,
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
        hasFieldValues: host.hasFieldValues,
      }),
    },
    {
      key: 'callstack',
      title: 'Callstack',
      content: m(CallstackView, {
        trace,
        dump: activeDump,
        state: host.callstackPanelState,
        onStateChange: host.setCallstackPanelState.bind(host),
      }),
    },
  ];

  // Static tab keys are view names.
  for (const tab of tabs) {
    actions.set(tab.key, {select: () => host.navigate(tab.key as NavView)});
  }

  for (const fg of host.flamegraphTabs) {
    const key = fgTabKey(fg.pathHashes, fg.isDominator);
    const title =
      fg.count !== null
        ? `Flamegraph objects (${fg.count.toLocaleString()})`
        : 'Flamegraph objects';
    tabs.push({
      key,
      title,
      closeButton: true,
      content: m(FlamegraphObjectsView, {
        engine,
        navigate: navigateWithTabs,
        potentiallyMergedClasses,
        pathHashes: fg.pathHashes,
        isDominator: fg.isDominator,
        onBackToTimeline: () => trace.navigate('#!/viewer'),
      }),
    });
    actions.set(key, {
      select: () =>
        host.navigate('flamegraph-objects', {
          pathHashes: fg.pathHashes,
          isDominator: fg.isDominator,
        }),
      close: () => host.closeFlamegraph(fg.pathHashes, fg.isDominator),
    });
  }

  for (const obj of host.instanceTabs) {
    const key = instanceTabKey(obj.objId);
    tabs.push({
      key,
      title: obj.label,
      closeButton: true,
      content: m(ObjectView, {
        engine,
        activeDump,
        navigate: navigateWithTabs,
        openFlamegraphPivotedAt: host.openFlamegraphPivotedAt.bind(host),
        params: {id: obj.objId},
        potentiallyMergedClasses,
      }),
    });
    actions.set(key, {
      select: () => host.navigate('object', {id: obj.objId}),
      close: () => host.closeInstanceTab(obj.objId),
    });
  }

  // Lazy: a view only mounts (and runs its queries) once its tab is first
  // opened, rather than every view loading when the page opens.
  return {tabs: tabs.map((t) => ({...t, lazy: true})), actions};
}

export function processLabel(d: HeapDump): string {
  const name = d.processName ?? '<unknown>';
  return d.pid !== null ? `${name} (pid ${d.pid})` : name;
}

export function renderDumpSelector(
  trace: Trace,
  allDumps: ReadonlyArray<HeapDump>,
  active: HeapDump | null,
  onSelect: (d: HeapDump) => void,
): m.Children {
  if (allDumps.length <= 1 || active === null) return null;

  return m(
    '.pf-hde-dump-selector',
    m('span.pf-hde-dump-selector__label', 'Heap dump:'),
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
        const offset = Time.diff(d.ts, trace.traceInfo.start);
        return m(MenuItem, {
          label: `${processLabel(d)} — ${formatDuration(trace, offset)}`,
          active: d === active,
          onclick: () => onSelect(d),
        });
      }),
    ),
  );
}
