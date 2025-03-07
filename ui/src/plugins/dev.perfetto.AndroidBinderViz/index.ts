// Copyright (C) 2023 The Android Open Source Project
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

import {
  BreakdownTrackAggType,
  BreakdownTracks,
} from '../../components/tracks/breakdown_tracks';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.AndroidBinderVizPlugin';

  async onTraceLoad(ctx: Trace): Promise<void> {
    const binderCounterBreakdowns = new BreakdownTracks({
      trace: ctx,
      trackTitle: 'Binder Transaction Counts',
      modules: ['android.binder', 'android.binder_breakdown'],
      aggregationType: BreakdownTrackAggType.COUNT,
      aggregation: {
        columns: [
          'server_process',
          '(IFNULL(interface, "unknown"))',
          '(IFNULL(method_name, "unknown"))',
          '(client_process || ":" || client_upid)',
          '(client_thread || ":" ||  client_utid)',
        ],
        tsCol: 'client_ts',
        durCol: 'client_dur',
        tableName: 'android_binder_txns',
      },
      slice: {
        columns: ['aidl_name'],
        tableName: 'android_binder_txns',
        tsCol: 'client_ts',
        durCol: 'client_dur',
      },
      pivots: {
        columns: ['reason_type', 'reason'],
        tableName: 'android_binder_client_server_breakdown',
        tsCol: 'ts',
        durCol: 'dur',
        joins: [
          {
            joinTableName: 'android_binder_client_server_breakdown',
            joinColumns: ['binder_txn_id'],
          },
        ],
      },
    });

    ctx.workspace.addChildInOrder(await binderCounterBreakdowns.createTracks());
  }
}
