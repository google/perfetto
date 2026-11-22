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

import './sysmon.scss';
import m from 'mithril';
import type {App} from '../../../ui/src/public/app';
import type {PerfettoPlugin} from '../../../ui/src/public/plugin';
import LlmPlugin from '../../../ui/src/plugins/dev.perfetto.Llm';
import LlmProtocolOpenAiPlugin from '../../../ui/src/plugins/dev.perfetto.LlmProtocolOpenAi';
import RecordPageV2 from '../../../ui/src/plugins/dev.perfetto.RecordTraceV2';
import {mountProductAgentSidebar} from '../../common/ui/agent/product_agent_plugin';
import {LiveView} from './live_view';
import {SnapshotWorkspace} from './snapshot_workspace';
import {getSysmonSqlPackages, sysmonState} from './state';
import {buildSysmonAgentAdapter} from './sysmon_agent';
import {SnapshotCompareView} from './views/compare_view';

export const SYSMON_PLUGIN_ID = 'perfetto.product.sysmon';

export default class SysmonPlugin implements PerfettoPlugin {
  static readonly id = SYSMON_PLUGIN_ID;
  static readonly description =
    'Whole-device system resource profiler with Live vitals, instant 60s Snapshots, and progressive explainers.';
  static readonly dependencies = [
    RecordPageV2,
    LlmPlugin,
    LlmProtocolOpenAiPlugin,
  ];

  static onActivate(app: App): void {
    sysmonState.attachApp(app);
    app.addSqlPackages(getSysmonSqlPackages());
    mountProductAgentSidebar(app, buildSysmonAgentAdapter());

    // Optional ?memscopeUrl=... override for local multi-product dev servers.
    const initArgs = app.getCurrentRoute().args;
    const msOverride = initArgs['memscopeUrl'];
    if (typeof msOverride === 'string' && msOverride.trim() !== '') {
      localStorage.setItem('sysmon.memscopeUrl', msOverride.trim());
    }

    // Track whether we already auto-loaded a ?traceUrl= or ?liveTraceUrl= param.
    let loadedTraceUrl: string | undefined;
    let loadedLiveTraceUrl: string | undefined;

    app.pages.registerPage({
      route: '/sysmon',
      render: () => {
        const args = app.getCurrentRoute().args;
        const traceUrl = args['traceUrl'];
        if (
          typeof traceUrl === 'string' &&
          traceUrl !== '' &&
          traceUrl !== loadedTraceUrl
        ) {
          loadedTraceUrl = traceUrl;
          void sysmonState.openTraceUrl(traceUrl);
        }
        const liveTraceUrl = args['liveTraceUrl'];
        if (
          typeof liveTraceUrl === 'string' &&
          liveTraceUrl !== '' &&
          liveTraceUrl !== loadedLiveTraceUrl
        ) {
          loadedLiveTraceUrl = liveTraceUrl;
          void sysmonState.openLivePreviewUrl(liveTraceUrl);
        }

        const viewArg = typeof args['view'] === 'string' ? args['view'] : undefined;
        if (viewArg === 'live') {
          sysmonState.activeView = {kind: 'live'};
        } else if (viewArg === 'compare' && sysmonState.snapshots.length >= 2) {
          sysmonState.activeView = {kind: 'compare'};
        } else if (
          viewArg &&
          sysmonState.snapshots.some((s) => s.id === viewArg)
        ) {
          sysmonState.activeView = {kind: 'snapshot', id: viewArg};
        }

        if (sysmonState.activeView.kind === 'compare') {
          return m(SnapshotCompareView);
        }
        if (sysmonState.activeView.kind === 'snapshot') {
          const snap = sysmonState.activeSnapshot;
          if (snap) {
            return m(SnapshotWorkspace, {key: snap.id, snap});
          }
        }
        return m(LiveView, {app});
      },
    });
  }
}
