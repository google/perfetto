// Copyright (C) 2025 The Android Open Source Project
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
import type {Trace} from '../../public/trace';
import {EmptyState} from '../../widgets/empty_state';
import {TabStrip} from '../../widgets/tab_strip';
import {Router} from '../../widgets/router';
import type {TabKey} from './utils';
import {OverviewTab} from './tabs/overview';
import {type OverviewData, loadOverviewData} from './tabs/overview_data';
import {ConfigTab, type ConfigData, loadConfigData} from './tabs/config';
import {
  AndroidTab,
  type AndroidData,
  loadAndroidData,
  hasAndroidData,
} from './tabs/android';
import {
  MachinesTab,
  type MachinesData,
  loadMachinesData,
} from './tabs/machines';
import {TracesTab, type TracesData, loadTracesData} from './tabs/traces';
import {
  ImportErrorsTab,
  type ImportErrorsData,
  loadImportErrorsData,
} from './tabs/import_errors';
import {
  DataLossesTab,
  type DataLossesData,
  loadDataLossesData,
} from './tabs/data_losses';
import {
  TraceErrorsTab,
  type TraceErrorsData,
  loadTraceErrorsData,
} from './tabs/trace_errors';
import {NoticesTab, type NoticesData, loadNoticesData} from './tabs/notices';
import {
  UiLoadingErrorsTab,
  type UiLoadingErrorsData,
} from './tabs/ui_loading_errors';
import {StatsTab, type StatsData, loadStatsData} from './tabs/stats';
import {
  TraceDoctorTab,
  type Diagnostic,
  loadTraceDiagnostics,
} from './diagnostics';
import {
  MetadataTab,
  type MetadataData,
  loadMetadataData,
  hasMetadataData,
} from './tabs/metadata';
import {tabHref} from './nav';
import {AsyncMemo} from '../../base/async_memo';

export interface TraceInfoPageAttrs {
  readonly trace: Trace;
  readonly subpage: string | undefined;
}

interface AllTabData {
  overview: OverviewData;
  diagnostics: ReadonlyArray<Diagnostic>;
  config: ConfigData;
  android: AndroidData;
  machines: MachinesData;
  traces: TracesData;
  metadata: MetadataData;
  importErrors: ImportErrorsData;
  traceErrors: TraceErrorsData;
  dataLosses: DataLossesData;
  notices: NoticesData;
  uiLoadingErrors: UiLoadingErrorsData;
  stats: StatsData;
}

export class TraceInfoPage implements m.ClassComponent<TraceInfoPageAttrs> {
  private tabDataMemo = new AsyncMemo<AllTabData>();

  view({attrs}: m.CVnode<TraceInfoPageAttrs>) {
    const {trace} = attrs;

    const {data} = this.tabDataMemo.use({
      key: {},
      compute: () => loadAllData(trace),
    });

    // One route per tab. `tab()` wraps each tab's content with the tab bar and
    // the loading / no-data states, so each route only says what it renders.
    const renderTab = (
      key: TabKey,
      render: (data: AllTabData) => m.Children,
    ) => {
      return [
        this.renderTabStrip(key, data),
        data === undefined
          ? m(EmptyState, {
              icon: 'hourglass_empty',
              title: 'Loading trace info...',
            })
          : render(data),
      ];
    };

    // Declare once as this tab is reused across multiple routes
    const renderOverviewTab = () =>
      renderTab('overview', (data) =>
        m(OverviewTab, {
          trace,
          data: data.overview,
          diagnostics: data.diagnostics,
        }),
      );

    return m(
      '.pf-trace-info-page',
      m(
        '.pf-trace-info-page__inner',
        m(
          '.pf-trace-info-page__header',
          m('h1.pf-trace-info-page__header-title', 'Overview'),
          m(
            '.pf-trace-info-page__subtitle',
            'High-level summary of trace health, metrics, and system information',
          ),
        ),
        m(Router, {
          path: attrs.subpage ?? '',
          routes: {
            '': () => renderOverviewTab(),
            'overview': () => renderOverviewTab(),
            'config': () =>
              renderTab('config', (data) => m(ConfigTab, {data: data.config})),
            'import_errors': () =>
              renderTab('import_errors', (data) =>
                m(ImportErrorsTab, {data: data.importErrors}),
              ),
            'trace_errors': () =>
              renderTab('trace_errors', (data) =>
                m(TraceErrorsTab, {data: data.traceErrors}),
              ),
            'trace_doctor': () =>
              renderTab('trace_doctor', (data) =>
                m(TraceDoctorTab, {
                  diagnostics: data.diagnostics,
                  isMultiTrace: data.overview.traceCount > 1,
                }),
              ),
            'data_losses': () =>
              renderTab('data_losses', (data) =>
                m(DataLossesTab, {data: data.dataLosses}),
              ),
            'notices': () =>
              renderTab('notices', (data) =>
                m(NoticesTab, {data: data.notices}),
              ),
            'ui_loading_errors': () =>
              renderTab('ui_loading_errors', (data) =>
                m(UiLoadingErrorsTab, {data: data.uiLoadingErrors}),
              ),
            'android': () =>
              renderTab('android', (data) =>
                m(AndroidTab, {data: data.android}),
              ),
            'traces': () =>
              renderTab('traces', (data) => m(TracesTab, {data: data.traces})),
            'machines': () =>
              renderTab('machines', (data) =>
                m(MachinesTab, {data: data.machines}),
              ),
            'metadata': () =>
              renderTab('metadata', (data) =>
                m(MetadataTab, {data: data.metadata}),
              ),
            'stats': () =>
              renderTab('stats', (data) => m(StatsTab, {data: data.stats})),
          },
          fallback: () => [
            this.renderTabStrip(undefined, data),
            m(EmptyState, {title: 'Page not found'}),
          ],
        }),
      ),
    );
  }

  // Renders the tab bar, highlighting `activeKey` if set.
  private renderTabStrip(
    activeKey: TabKey | undefined,
    data: AllTabData | undefined,
  ): m.Children {
    const propsForTab = (key: TabKey) => {
      return {
        active: activeKey === key,
        href: tabHref(key),
      };
    };
    return m(TabStrip, {variant: 'underline'}, [
      m(TabStrip.Link, propsForTab('overview'), 'Overview'),
      data && [
        data.config.configs.length !== 0 &&
          m(TabStrip.Link, propsForTab('config'), 'Trace Config'),
        data.overview.importErrors !== 0 &&
          m(TabStrip.Link, propsForTab('import_errors'), 'Import Errors'),
        data.traceErrors.errors.length !== 0 &&
          m(TabStrip.Link, propsForTab('trace_errors'), 'Trace Errors'),
        data.diagnostics.length !== 0 &&
          m(TabStrip.Link, propsForTab('trace_doctor'), 'Trace Doctor'),
        data.overview.dataLosses !== 0 &&
          m(TabStrip.Link, propsForTab('data_losses'), 'Data Losses'),
        data.notices.categories.length !== 0 &&
          m(TabStrip.Link, propsForTab('notices'), 'Notices'),
        data.overview.uiLoadingErrorCount !== 0 &&
          m(
            TabStrip.Link,
            propsForTab('ui_loading_errors'),
            'UI Loading Errors',
          ),
        hasAndroidData(data.android) &&
          m(TabStrip.Link, propsForTab('android'), 'Android'),
        data.overview.traceCount > 1 &&
          m(TabStrip.Link, propsForTab('traces'), 'Traces'),
        data.machines.machineCount > 1 &&
          m(TabStrip.Link, propsForTab('machines'), 'Machines'),
        hasMetadataData(data.metadata) &&
          m(TabStrip.Link, propsForTab('metadata'), 'Metadata'),
      ],
      m(TabStrip.Link, propsForTab('stats'), 'Statistics'),
    ]);
  }
}

async function loadAllData(trace: Trace): Promise<AllTabData> {
  const engine = trace.engine;
  return {
    overview: await loadOverviewData(trace),
    diagnostics: await loadTraceDiagnostics(engine),
    config: await loadConfigData(engine),
    android: await loadAndroidData(engine),
    machines: await loadMachinesData(engine),
    traces: await loadTracesData(engine),
    metadata: await loadMetadataData(engine),
    importErrors: await loadImportErrorsData(engine),
    traceErrors: await loadTraceErrorsData(engine),
    dataLosses: await loadDataLossesData(engine),
    notices: await loadNoticesData(engine),
    uiLoadingErrors: {errors: trace.loadingErrors},
    stats: await loadStatsData(engine),
  };
}
