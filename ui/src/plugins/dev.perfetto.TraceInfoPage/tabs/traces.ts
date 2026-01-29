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
import {Engine} from '../../../trace_processor/engine';
import {NUM_NULL, STR_NULL} from '../../../trace_processor/query_result';
import {Section} from '../../../widgets/section';
import {Grid, GridCell, GridHeaderCell} from '../../../widgets/grid';

// Trace row spec and type
const traceRowSpec = {
  traceId: NUM_NULL,
  uniqueSessionName: STR_NULL,
  traceUuid: STR_NULL,
  traceType: STR_NULL,
  traceSizeBytes: NUM_NULL,
  traceTrigger: STR_NULL,
  machines: STR_NULL,
};

type TraceRow = typeof traceRowSpec;

export interface TracesData {
  readonly traces: TraceRow[];
}

export async function loadTracesData(engine: Engine): Promise<TracesData> {
  const result = await engine.query(`
    INCLUDE PERFETTO MODULE std.traceinfo.trace;

    select
      trace_id as traceId,
      unique_session_name as uniqueSessionName,
      trace_uuid as traceUuid,
      trace_type as traceType,
      trace_size_bytes as traceSizeBytes,
      trace_trigger as traceTrigger,
      machines
    from _metadata_by_trace
    order by trace_id;
  `);

  const traces: TraceRow[] = [];
  for (const it = result.iter(traceRowSpec); it.valid(); it.next()) {
    traces.push({
      traceId: it.traceId,
      uniqueSessionName: it.uniqueSessionName,
      traceUuid: it.traceUuid,
      traceType: it.traceType,
      traceSizeBytes: it.traceSizeBytes,
      traceTrigger: it.traceTrigger,
      machines: it.machines,
    });
  }

  return {traces};
}

export interface TracesTabAttrs {
  data: TracesData;
}

export class TracesTab implements m.ClassComponent<TracesTabAttrs> {
  view({attrs}: m.CVnode<TracesTabAttrs>) {
    return m(
      '.pf-trace-info-page__tab-content',
      m(
        Section,
        {
          title: 'Traces',
          subtitle: 'Information about individual traces in this session',
        },
        m(TraceListSection, {data: attrs.data.traces}),
      ),
    );
  }
}

interface TraceListSectionAttrs {
  data: TraceRow[];
}

class TraceListSection implements m.ClassComponent<TraceListSectionAttrs> {
  view({attrs}: m.CVnode<TraceListSectionAttrs>) {
    const data = attrs.data;
    if (data === undefined || data.length === 0) {
      return undefined;
    }

    const traceTables = data.map((row) => {
      const gridRows = [];
      for (const key of Object.keys(traceRowSpec)) {
        const value = row[key as keyof TraceRow];
        if (value !== undefined && value !== null) {
          gridRows.push([m(GridCell, key), m(GridCell, String(value))]);
        }
      }

      return m(
        '',
        m(
          'h3',
          `Trace ${row.traceId}${row.uniqueSessionName ? ': ' + row.uniqueSessionName : row.traceType ? ': ' + row.traceType : ''}`,
        ),
        m(Grid, {
          columns: [
            {
              key: 'name',
              header: m(GridHeaderCell, 'Name'),
            },
            {
              key: 'value',
              header: m(GridHeaderCell, 'Value'),
            },
          ],
          rowData: gridRows,
          className: 'pf-trace-info-page__logs-grid',
        }),
      );
    });

    return traceTables;
  }
}
