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

import './styles.scss';
import m from 'mithril';
import {z} from 'zod';
import type {App} from '../../public/app';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Setting} from '../../public/settings';
import type {Trace} from '../../public/trace';
import RecordPageV2 from '../dev.perfetto.RecordTraceV2';
import {ConnectionPage} from './views/connection';
import {Dashboard} from './views/dashboard';
import {LiveSession} from './sessions/live_session';
import {MemoryOverviewPage} from './views/landing_page/memory_overview_page';
import {NUM} from '../../trace_processor/query_result';
import {EmptyState} from '../../widgets/empty_state';
import type {MemoryOverviewTab} from './views/landing_page/proc_mem_overview';
import {getBestProcess} from './views/landing_page/proc_mem_stats';

export default class MemscopePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Memscope';
  static readonly description =
    'Live memory profiler for Android/Linux devices';
  static readonly dependencies = [RecordPageV2];
  private static openByDefaultSetting: Setting<boolean>;
  private static hideDefaultChangedHintSetting: Setting<boolean>;

  static onActivate(app: App) {
    MemscopePlugin.openByDefaultSetting = app.settings.register({
      id: 'dev.perfetto.OpenMemoryOverviewByDefault',
      name: 'Open Memory Overview by default',
      description:
        'Open traces containing smaps snapshots in Memory Overview instead ' +
        'of the timeline.',
      schema: z.boolean(),
      defaultValue: true,
    });

    MemscopePlugin.hideDefaultChangedHintSetting = app.settings.register({
      id: 'dev.perfetto.HideMemoryOverviewDefaultChangedHint',
      name: 'Hide Memory Overview default-page explanation',
      description:
        'Do not show the explanation that Memory Overview is the default ' +
        'page for traces containing smaps snapshots.',
      schema: z.boolean(),
      defaultValue: false,
      headless: true,
    });

    let session: LiveSession | undefined;

    app.sidebar.addMenuItem({
      section: 'trace_files',
      text: 'Memscope',
      href: '#!/memscope',
      icon: 'memory',
      sortOrder: 2.5,
      badge: 'preview',
    });

    app.pages.registerPage({
      route: '/memscope',
      render: () => {
        if (session) {
          return m(Dashboard, {
            app,
            session,
            onStopped: () => {
              session?.dispose();
              session = undefined;
            },
          });
        } else {
          return m(ConnectionPage, {
            app,
            onConnected: (result) => {
              session = new LiveSession(app, result);
              session.onSnapshot(() => m.redraw());
            },
          });
        }
      },
    });
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    const pageRoot = '/memoryoverview';
    const openByDefault = MemscopePlugin.openByDefaultSetting;
    const hideDefaultChangedHint = MemscopePlugin.hideDefaultChangedHintSetting;
    const availability = await this.getMemoryOverviewAvailability(trace);
    const autoNavigated = openByDefault.get() && availability.hasSmapsSnapshots;
    const bestUpid = await getBestProcess(trace.engine);

    trace.pages.registerPage({
      route: pageRoot,
      render: (subpage) => {
        const {parsed, redirect} = resolveMemoryOverviewRoute(
          subpage,
          bestUpid,
          pageRoot,
        );
        if (redirect !== undefined) {
          return redirect;
        }

        return m(MemoryOverviewPage, {
          trace,
          upid: parsed.upid,
          tab: parsed.tab,
          autoNavigated,
          hdeAvailable: availability.hasHeapDumps,
          openByDefault,
          hideDefaultChangedHint,
          onUpidChange: (newUpid) => {
            trace.navigate(
              `#!${pageRoot}/${formatMemoryOverviewSubpage(newUpid, parsed.tab)}`,
            );
          },
          onTabChange: (tab) => {
            if (parsed.upid !== undefined) {
              trace.navigate(
                `#!${pageRoot}/${formatMemoryOverviewSubpage(parsed.upid, tab)}`,
              );
            }
          },
        });
      },
    });

    if (availability.hasSmapsSnapshots || availability.hasHeapDumps) {
      trace.sidebar.addMenuItem({
        section: 'current_trace',
        sortOrder: 25,
        text: 'Memory Overview',
        href: `#!${pageRoot}`,
        icon: 'memory',
        badge: 'preview',
      });
    }

    if (autoNavigated) {
      // Make this page appear before the heap dump explorer page.
      trace.initialPage.suggest(pageRoot, 500);
    }
  }

  private async getMemoryOverviewAvailability(trace: Trace): Promise<{
    readonly hasSmapsSnapshots: boolean;
    readonly hasHeapDumps: boolean;
  }> {
    const result = await trace.engine.query(`
      SELECT
        EXISTS(SELECT 1 FROM profiler_smaps) AS hasSmapsSnapshots,
        EXISTS(SELECT 1 FROM heap_graph) AS hasHeapDumps
    `);
    const row = result.firstRow({
      hasSmapsSnapshots: NUM,
      hasHeapDumps: NUM,
    });
    return {
      hasSmapsSnapshots: row.hasSmapsSnapshots !== 0,
      hasHeapDumps: row.hasHeapDumps !== 0,
    };
  }
}

interface MemoryOverviewSubpage {
  readonly upid?: number;
  readonly tab: MemoryOverviewTab;
}

function parseMemoryOverviewSubpage(subpage?: string): MemoryOverviewSubpage {
  if (!subpage) {
    return {tab: 'summary'};
  }
  const parts = subpage.split('/').filter((x) => x !== '');
  if (parts.length === 0) {
    return {tab: 'summary'};
  }
  const upid = parseInt(parts[0], 10);
  const tab = parts[1] === 'smaps' ? 'smaps' : 'summary';
  return {
    upid: Number.isNaN(upid) ? Number.NaN : upid,
    tab,
  };
}

function formatMemoryOverviewSubpage(
  upid: number,
  tab?: MemoryOverviewTab,
): string {
  if (tab === 'summary') {
    return `${upid}`;
  }

  if (tab !== undefined) {
    return `${upid}/${tab}`;
  }
  return `${upid}`;
}

function resolveMemoryOverviewRoute(
  subpage: string | undefined,
  bestUpid: number | undefined,
  pageRoot: string,
): {parsed: MemoryOverviewSubpage; redirect?: m.Children} {
  const parsed = parseMemoryOverviewSubpage(subpage);

  if (parsed.upid === undefined) {
    if (bestUpid !== undefined) {
      location.replace(
        `#!${pageRoot}/${formatMemoryOverviewSubpage(bestUpid, parsed.tab)}`,
      );
      return {
        parsed,
        redirect: m(EmptyState, {
          icon: 'hourglass',
          title: 'Loading process...',
        }),
      };
    }
    return {
      parsed,
      redirect: m(EmptyState, 'No processes with memory in this trace'),
    };
  }

  return {parsed};
}
