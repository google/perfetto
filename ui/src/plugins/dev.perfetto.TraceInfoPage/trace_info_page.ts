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
import {Trace} from '../../public/trace';
import {TabStrip, TabOption} from '../../widgets/tabs';
import {EmptyState} from '../../widgets/empty_state';
import type {TabKey} from './utils';
import {isValidTabKey} from './utils';
import {OverviewTab, OverviewData, loadOverviewData} from './tabs/overview';
import {ConfigTab, ConfigData, loadConfigData} from './tabs/config';
import {AndroidTab, AndroidData, loadAndroidData} from './tabs/android';
import {MachinesTab, MachinesData, loadMachinesData} from './tabs/machines';
import {
  ImportErrorsTab,
  ImportErrorsData,
  loadImportErrorsData,
} from './tabs/import_errors';
import {
  DataLossesTab,
  DataLossesData,
  loadDataLossesData,
} from './tabs/data_losses';
import {
  TraceErrorsTab,
  TraceErrorsData,
  loadTraceErrorsData,
} from './tabs/trace_errors';
import {
  UiLoadingErrorsTab,
  UiLoadingErrorsData,
} from './tabs/ui_loading_errors';
import {StatsTab, StatsData, loadStatsData} from './tabs/stats';

export interface TraceInfoPageAttrs {
  readonly trace: Trace;
  readonly subpage: string | undefined;
}

interface AllTabData {
  overview: OverviewData;
  config: ConfigData;
  android: AndroidData;
  machines: MachinesData;
  importErrors: ImportErrorsData;
  traceErrors: TraceErrorsData;
  dataLosses: DataLossesData;
  uiLoadingErrors: UiLoadingErrorsData;
  stats: StatsData;
}

export class TraceInfoPage implements m.ClassComponent<TraceInfoPageAttrs> {
  // All tab data
  private tabData?: AllTabData;
  private currentTab: TabKey = 'overview';
  private lastSubpage?: string;

  oninit({attrs}: m.CVnode<TraceInfoPageAttrs>) {
    this.loadAllData(attrs.trace);
  }

  view({attrs}: m.CVnode<TraceInfoPageAttrs>) {
    if (attrs.subpage !== this.lastSubpage) {
      this.lastSubpage = attrs.subpage;
      this.currentTab = getTab(attrs.subpage);
    }
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
        m(TabStrip, {
          tabs: this.getTabs(),
          currentTabKey: this.currentTab,
          onTabChange: (key: string) => {
            this.currentTab = isValidTabKey(key) ? key : 'overview';
          },
        }),
        this.renderCurrentTab(attrs.trace, this.currentTab),
      ),
    );
  }

  private renderCurrentTab(trace: Trace, currentTab: TabKey): m.Children {
    if (!this.tabData) {
      return m(EmptyState, {
        icon: 'hourglass_empty',
        title: 'Loading trace info...',
      });
    }
    switch (currentTab) {
      case 'overview':
        return m(OverviewTab, {
          trace,
          data: this.tabData.overview,
          onTabChange: (key: TabKey) => {
            this.currentTab = key;
          },
        });
      case 'config':
        return m(ConfigTab, {
          data: this.tabData.config,
        });
      case 'android':
        return m(AndroidTab, {
          data: this.tabData.android,
        });
      case 'machines':
        return m(MachinesTab, {
          data: this.tabData.machines,
        });
      case 'import_errors':
        return m(ImportErrorsTab, {
          data: this.tabData.importErrors,
        });
      case 'trace_errors':
        return m(TraceErrorsTab, {
          data: this.tabData.traceErrors,
        });
      case 'data_losses':
        return m(DataLossesTab, {
          data: this.tabData.dataLosses,
        });
      case 'ui_loading_errors':
        return m(UiLoadingErrorsTab, {
          data: this.tabData.uiLoadingErrors,
        });
      case 'stats':
        return m(StatsTab, {
          data: this.tabData.stats,
        });
    }
  }

  private async loadAllData(trace: Trace): Promise<void> {
    const engine = trace.engine;
    this.tabData = {
      overview: await loadOverviewData(trace),
      config: await loadConfigData(engine),
      android: await loadAndroidData(engine),
      machines: await loadMachinesData(engine),
      importErrors: await loadImportErrorsData(engine),
      traceErrors: await loadTraceErrorsData(engine),
      dataLosses: await loadDataLossesData(engine),
      uiLoadingErrors: {errors: trace.loadingErrors},
      stats: await loadStatsData(engine),
    };
    m.redraw();
  }

  private getTabs(): TabOption[] {
    const tabs: TabOption[] = [{key: 'overview', title: 'Overview'}];
    if (this.tabData?.config?.configText) {
      tabs.push({key: 'config', title: 'Trace Config'});
    }
    if ((this.tabData?.overview?.importErrors ?? 0) > 0) {
      tabs.push({key: 'import_errors', title: 'Import Errors'});
    }
    if ((this.tabData?.traceErrors?.errors?.length ?? 0) > 0) {
      tabs.push({key: 'trace_errors', title: 'Trace Errors'});
    }
    if ((this.tabData?.overview?.dataLosses ?? 0) > 0) {
      tabs.push({key: 'data_losses', title: 'Data Losses'});
    }
    if ((this.tabData?.overview?.uiLoadingErrorCount ?? 0) > 0) {
      tabs.push({key: 'ui_loading_errors', title: 'UI Loading Errors'});
    }
    const hasAndroid =
      (this.tabData?.android?.packageList?.length ?? 0) > 0 ||
      (this.tabData?.android?.gameInterventions?.length ?? 0) > 0;
    if (hasAndroid) {
      tabs.push({key: 'android', title: 'Android'});
    }
    if ((this.tabData?.machines?.machineCount ?? 0) > 1) {
      tabs.push({key: 'machines', title: 'Machines'});
    }
    tabs.push({key: 'stats', title: 'Info and Stats (advanced)'});
    return tabs;
  }
}

function getTab(subpage: string | undefined): TabKey {
  if (!subpage) {
    return 'overview';
  }
  const res = subpage.substring(1);
  return isValidTabKey(res) ? res : 'overview';
}
