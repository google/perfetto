// Copyright (C) 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import m from 'mithril';
import {Engine} from '../../../trace_processor/engine';
import {UNKNOWN} from '../../../trace_processor/query_result';
import {Section} from '../../../widgets/section';
import {Icon} from '../../../widgets/icon';
import {Tooltip} from '../../../widgets/tooltip';
import {statsSpec, StatsSectionRow} from '../utils';

// Trace metadata row spec and type
const traceMetadataRowSpec = {name: UNKNOWN, value: UNKNOWN};
type TraceMetadataRow = typeof traceMetadataRowSpec;

export interface StatsData {
  traceMetadata: TraceMetadataRow[];
  allStats: StatsSectionRow[];
}

export async function loadStatsData(engine: Engine): Promise<StatsData> {
  // Load all stats
  const allStatsResult = await engine.query(`
    select
      name,
      value,
      cast(ifnull(idx, '') as text) as idx,
      description,
      severity,
      source
    from stats
    order by name, idx
  `);
  const allStats: StatsSectionRow[] = [];
  for (const iter = allStatsResult.iter(statsSpec); iter.valid(); iter.next()) {
    allStats.push({
      name: iter.name,
      value: iter.value,
      description: iter.description,
      idx: iter.idx,
      severity: iter.severity,
      source: iter.source,
    });
  }

  // Load trace metadata
  const traceMetadataResult = await engine.query(`
    with metadata_with_priorities as (
      select
        name,
        ifnull(str_value, cast(int_value as text)) as value,
        name in (
          "trace_size_bytes",
          "cr-os-arch",
          "cr-os-name",
          "cr-os-version",
          "cr-physical-memory",
          "cr-product-version",
          "cr-hardware-class"
        ) as priority
      from metadata
    )
    select
      name,
      value
    from metadata_with_priorities
    order by
      priority desc,
      name
  `);
  const traceMetadata: TraceMetadataRow[] = [];
  for (
    const iter = traceMetadataResult.iter(traceMetadataRowSpec);
    iter.valid();
    iter.next()
  ) {
    traceMetadata.push({
      name: iter.name,
      value: iter.value,
    });
  }

  return {
    traceMetadata,
    allStats,
  };
}

export interface StatsTabAttrs {
  data: StatsData;
}

export class StatsTab implements m.ClassComponent<StatsTabAttrs> {
  view({attrs}: m.CVnode<StatsTabAttrs>) {
    return m(
      '.pf-trace-info-page__tab-content',
      m(
        Section,
        {
          title: 'Trace Metadata',
          subtitle: 'All metadata key-value pairs recorded in the trace',
        },
        m(TraceMetadata, {data: attrs.data.traceMetadata}),
      ),
      m(
        Section,
        {
          title: 'Statistics',
          subtitle:
            'Complete dump of all trace statistics including errors, data losses, and debugging info',
        },
        m(StatsSection, {data: attrs.data.allStats}),
      ),
    );
  }
}

// Trace Metadata Section
interface TraceMetadataAttrs {
  data: TraceMetadataRow[];
}

class TraceMetadata implements m.ClassComponent<TraceMetadataAttrs> {
  view({attrs}: m.CVnode<TraceMetadataAttrs>) {
    const data = attrs.data;
    if (data === undefined || data.length === 0) {
      return m('');
    }

    const tableRows = data.map((row) => {
      return m(
        'tr.pf-trace-info-page__stats-table-row',
        m(
          'td.pf-trace-info-page__stats-table-cell.pf-trace-info-page__stats-table-cell--name',
          `${row.name}`,
        ),
        m('td.pf-trace-info-page__stats-table-cell', `${row.value}`),
      );
    });

    return m(
      'section.pf-trace-info-page__stats-section',
      m(
        'table.pf-trace-info-page__stats-table',
        m(
          'thead',
          m(
            'tr',
            m('td.pf-trace-info-page__stats-table-head-cell', 'Name'),
            m('td.pf-trace-info-page__stats-table-head-cell', 'Value'),
          ),
        ),
        m('tbody', tableRows),
      ),
    );
  }
}

// Stats Section
interface StatsSectionAttrs {
  data: StatsSectionRow[];
}

class StatsSection implements m.ClassComponent<StatsSectionAttrs> {
  view({attrs}: m.CVnode<StatsSectionAttrs>) {
    const data = attrs.data;
    if (data === undefined || data.length === 0) {
      return m('');
    }

    const tableRows = data.map((row) => {
      const help = [];
      if (Boolean(row.description)) {
        help.push(
          m(
            Tooltip,
            {
              trigger: m(Icon, {
                icon: 'help_outline',
                className: 'pf-trace-info-page__help-icon',
              }),
            },
            `${row.description}`,
          ),
        );
      }
      const idx = row.idx !== '' ? `[${row.idx}]` : '';
      return m(
        'tr.pf-trace-info-page__stats-table-row',
        m(
          'td.pf-trace-info-page__stats-table-cell.pf-trace-info-page__stats-table-cell--name',
          `${row.name}${idx}`,
          help,
        ),
        m('td.pf-trace-info-page__stats-table-cell', `${row.value}`),
        m(
          'td.pf-trace-info-page__stats-table-cell',
          `${row.severity} (${row.source})`,
        ),
      );
    });

    return m(
      'section.pf-trace-info-page__stats-section',
      m(
        'table.pf-trace-info-page__stats-table',
        m(
          'thead',
          m(
            'tr',
            m('td.pf-trace-info-page__stats-table-head-cell', 'Name'),
            m('td.pf-trace-info-page__stats-table-head-cell', 'Value'),
            m('td.pf-trace-info-page__stats-table-head-cell', 'Type'),
          ),
        ),
        m('tbody', tableRows),
      ),
    );
  }
}
