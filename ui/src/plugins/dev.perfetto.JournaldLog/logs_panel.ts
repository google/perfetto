// Copyright (C) 2024 The Android Open Source Project
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
  LogPanel,
  LogPanelAttrs,
  LogPanelConfig,
  BaseLogFilteringCriteria,
  serializeTags,
} from '../../components/widgets/log_panel/log_panel';
import {Store} from '../../base/store';
import {Trace} from '../../public/trace';
import {GridCell, GridHeaderCell} from '../../widgets/grid';
import {escapeRegexQuery} from '../../trace_processor/query_utils';
import {STR} from '../../trace_processor/query_result';

export interface JournaldLogFilteringCriteria
  extends BaseLogFilteringCriteria {}

// JournaldLogPanelCache is intentionally empty (no machine IDs needed),
// but kept for API consistency with the Android log panel.
export interface JournaldLogPanelCache {}

export interface JournaldLogPanelAttrs {
  readonly cache: JournaldLogPanelCache;
  readonly filterStore: Store<JournaldLogFilteringCriteria>;
  readonly trace: Trace;
}

// Journald priorities: index = numeric syslog priority value.
export const JOURNALD_PRIORITIES = [
  'Emergency',
  'Alert',
  'Critical',
  'Error',
  'Warning',
  'Notice',
  'Info',
  'Debug',
];

function classForPriority(p: number): string | undefined {
  if (p <= 2) return 'fatal'; // EMERG, ALERT, CRIT
  if (p === 3) return 'error'; // ERR
  if (p === 4) return 'warn'; // WARNING
  if (p <= 6) return 'info'; // NOTICE, INFO
  if (p === 7) return 'debug';
  return undefined;
}

const JOURNALD_CONFIG: LogPanelConfig = {
  title: 'Journald Logs',
  priorities: JOURNALD_PRIORITIES,
  ignoredPriorityStates: 0,
  filteredTableName: 'filtered_journald_logs',
  classPrefix: 'pf-journald-logs-panel',
  classForPriority,
  buildSelectedRows: (filter, globMatch) => {
    let sql = `select prio, ts, pid, tid, tag, msg,
        process.name as process_name,
        ifnull(journald_logs.systemd_unit, '') as systemd_unit,
        thread.utid as utid, thread.upid as upid,
        ${globMatch}
        from journald_logs
        left join thread using(utid) left join process using(upid)
        where prio <= ${filter.minimumLevel}`;
    if (filter.tags.length) {
      sql += filter.isTagRegex
        ? ` and (${filter.tags.map((p) => `tag glob ${escapeRegexQuery(p)}`).join(' OR ')})`
        : ` and tag in (${serializeTags(filter.tags)})`;
    }
    return sql;
  },
  extraSelectColumns: ["ifnull(systemd_unit, '') as systemdUnit"],
  extraSchema: {systemdUnit: STR},
  collectExtraRow: (it, extra) => {
    if (extra['systemdUnits'] === undefined) extra['systemdUnits'] = [];
    extra['systemdUnits'].push(it.systemdUnit as string);
  },
  extraColumns: (entries) => {
    const units = entries.extra['systemdUnits'] as string[] | undefined;
    if (!units?.some((u) => u)) return [];
    return [{key: 'unit', header: m(GridHeaderCell, 'Unit')}];
  },
  renderExtraCells: (entries, i, className, _trace) => {
    const units = entries.extra['systemdUnits'] as string[] | undefined;
    if (!units?.some((u) => u)) return [];
    return [m(GridCell, {className}, units[i])];
  },
};

export class JournaldLogPanel
  implements m.ClassComponent<JournaldLogPanelAttrs>
{
  view({attrs}: m.CVnode<JournaldLogPanelAttrs>): m.Children {
    const panelAttrs: LogPanelAttrs = {
      config: JOURNALD_CONFIG,
      filterStore: attrs.filterStore as Store<BaseLogFilteringCriteria>,
      trace: attrs.trace,
    };
    return m(LogPanel, panelAttrs);
  }
}

// Keep the old export name for backwards compatibility with index.ts
export {JournaldLogPanel as LogPanel};
