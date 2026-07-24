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

import type {Trace} from '../../public/trace';
import {
  LONG_NULL,
  NUM_NULL,
  STR_NULL,
  type Row,
  type SqlValue,
  type QueryResult as DbQueryResult,
} from '../../trace_processor/query_result';
import {type duration, Time, Duration} from '../../base/time';
import {
  getTrackUriForTrackId,
  enrichDepths,
} from '../../components/related_events/utils';
import {QuerySlot, type QueryResult} from '../../base/query_slot';
import type {
  InputLifecycleExtension,
  CellData,
  NavTarget,
  StageDefinition,
} from './extensions/interface';

export interface InputChainRow {
  uiRowId: string;
  inputEventId: string | null;
  channel: string;
  totalLatency: duration | null;
  stagesData: Map<string, CellData>;
  allTrackUris: string[];
}

interface InputLifecycleSpec extends Row {
  input_id: string | null;
  channel: string | null;
  total_latency: bigint | null;
  [key: string]: SqlValue;
}

export class AndroidInputEventSource {
  private readonly dataSlot = new QuerySlot<InputChainRow[]>();

  constructor(
    private readonly trace: Trace,
    private readonly activeExtensions: ReadonlyArray<InputLifecycleExtension>,
  ) {}

  use(sliceId: number): QueryResult<InputChainRow[]> {
    return this.dataSlot.use({
      key: {sliceId},
      queryFn: async () => {
        const resolvedSliceId = await this.resolveSliceId(sliceId);
        const rows = await this.fetchRows(resolvedSliceId);
        await this.enrichAllDepths(rows);
        return rows;
      },
    });
  }

  /**
   * Resolves an extension-specific slice ID back to a core slice ID.
   *
   * If the clicked slice belongs to an extension, we resolve it to the framework
   * input event ID, and then look up the corresponding 'InputReader' slice ID.
   * This core slice ID is used to "root" the core lifecycle query.
   */
  private async resolveSliceId(sliceId: number): Promise<number> {
    for (const ext of this.activeExtensions) {
      if (!ext.resolveInputId) continue;

      const inputId = await ext.resolveInputId(this.trace, sliceId);
      if (!inputId) continue;

      // Map the framework input ID to the core InputReader notifyMotion slice ID.
      const coreSliceResult = await this.trace.engine.query(`
        SELECT s.id
        FROM android_input_events e
        JOIN slice s ON s.ts = e.read_time AND s.track_id != 0
        WHERE e.input_event_id = '${inputId}'
        LIMIT 1
      `);
      const it = coreSliceResult.iter({id: NUM_NULL});
      if (it.valid() && it.id !== null) {
        return it.id;
      }
    }
    return sliceId;
  }

  /**
   * Compiles the full sequence of lifecycle stage specifications by combining
   * the core framework stages with those injected by active extensions, and
   * sorting them chronologically based on their sequence numbers.
   */
  static getStageSpecs(
    activeExtensions: ReadonlyArray<InputLifecycleExtension>,
  ): StageDefinition[] {
    const specs = [...CORE_STAGES];
    for (const ext of activeExtensions) {
      for (const stage of ext.getStages()) {
        specs.push({
          ...stage,
          key: `${ext.id}-${stage.key}`,
        });
      }
    }
    return specs.sort((a, b) => a.sequenceNumber - b.sequenceNumber);
  }

  getStageSpecs(): StageDefinition[] {
    return AndroidInputEventSource.getStageSpecs(this.activeExtensions);
  }

  private async fetchRows(sliceId: number): Promise<InputChainRow[]> {
    const baseQuery = `SELECT * FROM _android_input_lifecycle_by_slice_id(${sliceId})`;

    let selectCols = 'core.*, a_evt.event_time';
    let joinSql = '';

    for (const ext of this.activeExtensions) {
      const spec = ext.getSqlJoinSpec();
      const alias = spec.tableAlias ?? spec.tableName;
      selectCols += `, ${alias}.*`;
      joinSql += ` LEFT JOIN ${spec.tableName}${spec.tableAlias ? ` AS ${spec.tableAlias}` : ''} ON ${spec.joinOn}`;
    }

    const sql = `
      SELECT ${selectCols}
      FROM (${baseQuery}) core
      LEFT JOIN android_input_events a_evt ON 
        core.input_id = a_evt.input_event_id AND
        core.channel = a_evt.event_channel
      ${joinSql}
    `;

    const result = await this.trace.engine.query(sql);
    return this.mapResultToRows(result);
  }

  async getRowsForApp(
    processName?: string,
    excludeSpeculative = false,
  ): Promise<InputChainRow[]> {
    const processFilter = processName ? `'${processName}'` : 'NULL';
    const speculativeFilter = excludeSpeculative ? '1' : '0';

    const baseQuery = `
      SELECT
        e.input_event_id AS input_id,
        e.event_channel AS channel,
        e.end_to_end_latency_dur AS total_latency,
        e.event_time AS event_time,
        e.read_time AS ts_reader,
        e.dispatch_ts AS ts_dispatch,
        e.receive_ts AS ts_receive,
        s_cons.ts AS ts_consume,
        s_frame.ts AS ts_frame,
        s_read.id AS id_reader,
        s_read.track_id AS track_reader,
        s_read.dur AS dur_reader,
        s_disp.id AS id_dispatch,
        e.dispatch_track_id AS track_dispatch,
        s_disp.dur AS dur_dispatch,
        s_recv.id AS id_receive,
        e.receive_track_id AS track_receive,
        s_recv.dur AS dur_receive,
        s_cons.id AS id_consume,
        s_cons.track_id AS track_consume,
        s_cons.dur AS dur_consume,
        s_frame.id AS id_frame,
        s_frame.track_id AS track_frame,
        s_frame.dur AS dur_frame,
        e.is_speculative_frame
      FROM android_input_events AS e
      LEFT JOIN slice AS s_read
        ON s_read.ts = e.read_time
        AND s_read.track_id != 0
        AND s_read.name GLOB 'UnwantedInteractionBlocker::notifyMotion*'
      LEFT JOIN slice AS s_disp
        ON s_disp.ts = e.dispatch_ts
        AND s_disp.track_id = e.dispatch_track_id
      LEFT JOIN slice AS s_recv
        ON s_recv.ts = e.receive_ts
        AND s_recv.track_id = e.receive_track_id
      LEFT JOIN _input_consumers_lookup AS s_cons
        ON s_cons.cookie = e.event_seq
      LEFT JOIN _frame_choreographer_lookup AS s_frame
        ON s_frame.frame_id = CAST(e.frame_id AS LONG)
      WHERE e.event_channel LIKE '%/%'
        AND (e.process_name = ${processFilter} OR ${processFilter} IS NULL)
        AND (${speculativeFilter} = 0 OR e.is_speculative_frame = 0 OR e.is_speculative_frame IS NULL)
    `;

    let selectCols = 'a_evt.*';
    let joinSql = '';

    for (const ext of this.activeExtensions) {
      const spec = ext.getSqlJoinSpec();
      const alias = spec.tableAlias ?? spec.tableName;
      selectCols += `, ${alias}.*`;
      joinSql += ` LEFT JOIN ${spec.tableName}${spec.tableAlias ? ` AS ${spec.tableAlias}` : ''} ON ${spec.joinOn}`;
    }

    const sql = `
      SELECT ${selectCols}
      FROM (${baseQuery}) a_evt
      ${joinSql}
    `;

    const result = await this.trace.engine.query(sql);
    const rows = this.mapResultToRows(result);
    await this.enrichAllDepths(rows);
    return rows;
  }

  private mapResultToRows(result: DbQueryResult): InputChainRow[] {
    const rows: InputChainRow[] = [];
    let index = 0;

    const stages = this.getStageSpecs();
    const spec: InputLifecycleSpec = {
      input_id: STR_NULL,
      channel: STR_NULL,
      total_latency: LONG_NULL,
    };
    for (const stage of stages) {
      spec[stage.idField] = NUM_NULL;
      spec[stage.trackField] = NUM_NULL;
      spec[stage.tsField] = LONG_NULL;
      spec[stage.durField] = LONG_NULL;
    }

    const it = result.iter(spec);

    while (it.valid()) {
      const stagesData = new Map<string, CellData>();
      const allTrackUris: string[] = [];

      for (const stage of stages) {
        const id = it.get(stage.idField) as number | null;
        const trackId = it.get(stage.trackField) as number | null;
        const ts = it.get(stage.tsField) as bigint | null;
        const dur = (it.get(stage.durField) as bigint | null) ?? 0n;

        const cellDur =
          it.get(stage.durField) !== null ? Duration.fromRaw(dur) : null;

        let nav: NavTarget | undefined = undefined;
        if (id !== null && trackId !== null && ts !== null) {
          nav = {
            id,
            trackUri: getTrackUriForTrackId(this.trace, trackId),
            ts: Time.fromRaw(ts),
            dur: Duration.fromRaw(dur),
            depth: 0,
          };
          allTrackUris.push(nav.trackUri);
        }

        stagesData.set(stage.key, {dur: cellDur, nav});
      }

      // TODO(ivankc) Consider how to properly handle this in the context of extensions.
      const totalLatency =
        it.total_latency !== null ? Duration.fromRaw(it.total_latency) : null;

      rows.push({
        uiRowId: `row-${index++}`,
        inputEventId: it.input_id,
        channel: it.channel ?? '',
        totalLatency,
        stagesData,
        allTrackUris,
      });

      it.next();
    }

    return rows;
  }

  private async enrichAllDepths(rows: InputChainRow[]) {
    const targets: NavTarget[] = [];
    for (const row of rows) {
      for (const stageData of row.stagesData.values()) {
        if (stageData.nav) {
          targets.push(stageData.nav);
        }
      }
    }
    if (targets.length === 0) return;
    await enrichDepths(this.trace, targets);
  }
}

const CORE_STAGES: StageDefinition[] = [
  {
    key: 'read',
    headerName: 'InputReader',
    sequenceNumber: 1000,
    idField: 'id_reader',
    trackField: 'track_reader',
    tsField: 'ts_reader',
    durField: 'dur_reader',
  },
  {
    key: 'disp',
    headerName: 'Dispatcher',
    sequenceNumber: 2000,
    idField: 'id_dispatch',
    trackField: 'track_dispatch',
    tsField: 'ts_dispatch',
    durField: 'dur_dispatch',
  },
  {
    key: 'recv',
    headerName: 'App Receive',
    sequenceNumber: 3000,
    idField: 'id_receive',
    trackField: 'track_receive',
    tsField: 'ts_receive',
    durField: 'dur_receive',
  },
  {
    key: 'cons',
    headerName: 'App Consume',
    sequenceNumber: 4000,
    idField: 'id_consume',
    trackField: 'track_consume',
    tsField: 'ts_consume',
    durField: 'dur_consume',
  },
  {
    key: 'frame',
    headerName: 'App Frame',
    sequenceNumber: 5000,
    idField: 'id_frame',
    trackField: 'track_frame',
    tsField: 'ts_frame',
    durField: 'dur_frame',
  },
];
