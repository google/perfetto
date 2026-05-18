// Copyright (C) 2019 The Android Open Source Project
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
import {
  LogPanel as SharedLogPanel,
  type LogPanelConfig,
  type BaseLogFilteringCriteria,
  type LogPanelAttrs as SharedLogPanelAttrs,
  serializeTags,
} from '../../components/widgets/log_panel/log_panel';
import type {Store} from '../../base/store';
import type {Trace} from '../../public/trace';
import {escapeRegexQuery} from '../../trace_processor/query_utils';
import {
  type MultiSelectDiff,
  type MultiSelectOption,
  PopupMultiSelect,
} from '../../widgets/multiselect';
import {PopupPosition} from '../../widgets/popup';
import {Icons} from '../../base/semantic_icons';

export interface LogFilteringCriteria extends BaseLogFilteringCriteria {
  readonly machineExcludeList: number[];
}

export interface LogPanelCache {
  readonly uniqueMachineIds: number[];
}

export interface LogPanelAttrs {
  readonly cache: LogPanelCache;
  readonly filterStore: Store<LogFilteringCriteria>;
  readonly trace: Trace;
}

// Android log priorities: index = numeric priority value.
const ANDROID_PRIORITIES = [
  '-',
  '-',
  'Verbose',
  'Debug',
  'Info',
  'Warn',
  'Error',
  'Fatal',
];

function classForPriority(p: number): string | undefined {
  switch (p) {
    case 2:
      return 'verbose';
    case 3:
      return 'debug';
    case 4:
      return 'info';
    case 5:
      return 'warn';
    case 6:
      return 'error';
    case 7:
      return 'fatal';
    default:
      return undefined;
  }
}

function buildAndroidConfig(
  cache: LogPanelCache,
  filterStore: Store<LogFilteringCriteria>,
): LogPanelConfig {
  return {
    title: 'Android Logs',
    priorities: ANDROID_PRIORITIES,
    ignoredPriorityStates: 2,
    filteredTableName: 'filtered_logs',
    classPrefix: 'pf-logs-panel',
    classForPriority,
    buildSelectedRows: (
      filter: BaseLogFilteringCriteria,
      globMatch: string,
    ) => {
      const androidFilter = filter as LogFilteringCriteria;
      let sql = `select android_logs.id, prio, ts, pid, tid, tag, msg,
          process.name as process_name,
          thread.utid as utid, thread.upid as upid,
          ${globMatch}
          from android_logs
          left join thread using(utid)
          left join process using(upid)
          where prio >= ${filter.minimumLevel}`;
      if (filter.tags.length) {
        sql += filter.isTagRegex
          ? ` and (${filter.tags.map((p) => `tag glob ${escapeRegexQuery(p)}`).join(' OR ')})`
          : ` and tag in (${serializeTags(filter.tags)})`;
      }
      if (androidFilter.machineExcludeList?.length) {
        sql += ` and process.machine_id not in (${androidFilter.machineExcludeList.join(',')})`;
      }
      return sql;
    },
    renderExtraFilters: (store: Store<BaseLogFilteringCriteria>) => {
      const hasMachineIds = cache.uniqueMachineIds.length > 1;
      if (!hasMachineIds) return null;

      const androidStore = store as Store<LogFilteringCriteria>;
      const machineExcludeList = androidStore.state.machineExcludeList ?? [];
      const options: MultiSelectOption[] = cache.uniqueMachineIds.map(
        (uMachineId) => ({
          id: String(uMachineId),
          name: `Machine ${uMachineId}`,
          checked: !machineExcludeList.some((x) => x === uMachineId),
        }),
      );

      return m(PopupMultiSelect, {
        label: 'Filter by machine',
        icon: Icons.Filter,
        position: PopupPosition.Top,
        options,
        onChange: (diffs: MultiSelectDiff[]) => {
          const newList = new Set<number>(machineExcludeList);
          diffs.forEach(({checked, id}) => {
            const machineId = Number(id);
            if (checked) {
              newList.delete(machineId);
            } else {
              newList.add(machineId);
            }
          });
          filterStore.edit((draft) => {
            draft.machineExcludeList = Array.from(newList);
          });
        },
      });
    },
  };
}

export class LogPanel implements m.ClassComponent<LogPanelAttrs> {
  view({attrs}: m.CVnode<LogPanelAttrs>): m.Children {
    const config = buildAndroidConfig(attrs.cache, attrs.filterStore);
    const panelAttrs: SharedLogPanelAttrs = {
      config,
      filterStore: attrs.filterStore as Store<BaseLogFilteringCriteria>,
      trace: attrs.trace,
    };
    return m(SharedLogPanel, panelAttrs);
  }
}
