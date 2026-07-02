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

    const rows: InputChainRow[] = [];
    let index = 0;

    const it = result.iter({
      input_id: STR_NULL,
      channel: STR_NULL,
      total_latency: LONG_NULL,

      ts_reader: LONG_NULL,
      id_reader: NUM_NULL,
      track_reader: NUM_NULL,
      dur_reader: LONG_NULL,

      ts_dispatch: LONG_NULL,
      id_dispatch: NUM_NULL,
      track_dispatch: NUM_NULL,
      dur_dispatch: LONG_NULL,

      ts_receive: LONG_NULL,
      id_receive: NUM_NULL,
      track_receive: NUM_NULL,
      dur_receive: LONG_NULL,

      ts_consume: LONG_NULL,
      id_consume: NUM_NULL,
      track_consume: NUM_NULL,
      dur_consume: LONG_NULL,

      ts_frame: LONG_NULL,
      id_frame: NUM_NULL,
      track_frame: NUM_NULL,
      dur_frame: LONG_NULL,
    });

    const activeStages = [...CORE_STAGES];
    for (const ext of this.activeExtensions) {
      for (const stage of ext.getStages()) {
        activeStages.push({
          ...stage,
          key: `${ext.id}-${stage.key}`,
        });
      }
    }
    activeStages.sort((a, b) => a.sequenceNumber - b.sequenceNumber);

    while (it.valid()) {
      const stagesData = new Map<string, CellData>();
      const allTrackUris: string[] = [];
      let minTs: bigint | null = null;
      let maxTs: bigint | null = null;

      for (const stage of activeStages) {
        const id = it.get(stage.idField) as number | null;
        const trackId = it.get(stage.trackField) as number | null;
        const ts = it.get(stage.tsField) as bigint | null;
        const dur = (it.get(stage.durField) as bigint | null) ?? 0n;

        let cellDur: duration | null = null;
        if (stage.prevTsField) {
          const prevTs = it.get(stage.prevTsField) as bigint | null;
          cellDur =
            ts !== null && prevTs !== null
              ? Duration.fromRaw(ts - prevTs)
              : null;
        } else {
          cellDur =
            it.get(stage.durField) !== null ? Duration.fromRaw(dur) : null;
        }

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

          // Track min/max timestamps of execution span for total latency
          minTs = minTs === null || ts < minTs ? ts : minTs;
          const endTs = ts + dur;
          maxTs = maxTs === null || endTs > maxTs ? endTs : maxTs;
        }

        stagesData.set(stage.key, {dur: cellDur, nav});
      }

      const totalLatency =
        minTs !== null && maxTs !== null
          ? Duration.fromRaw(maxTs - minTs)
          : null;

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
    prevTsField: 'ts_reader',
  },
  {
    key: 'recv',
    headerName: 'App Receive',
    sequenceNumber: 3000,
    idField: 'id_receive',
    trackField: 'track_receive',
    tsField: 'ts_receive',
    durField: 'dur_receive',
    prevTsField: 'ts_dispatch',
  },
  {
    key: 'cons',
    headerName: 'App Consume',
    sequenceNumber: 4000,
    idField: 'id_consume',
    trackField: 'track_consume',
    tsField: 'ts_consume',
    durField: 'dur_consume',
    prevTsField: 'ts_receive',
  },
  {
    key: 'frame',
    headerName: 'App Frame',
    sequenceNumber: 5000,
    idField: 'id_frame',
    trackField: 'track_frame',
    tsField: 'ts_frame',
    durField: 'dur_frame',
    prevTsField: 'ts_consume',
  },
];
