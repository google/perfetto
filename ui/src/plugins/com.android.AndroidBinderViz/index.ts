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

import {
  BreakdownTrackAggType,
  BreakdownTracks,
} from '../../components/tracks/breakdown_tracks';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidBinderViz';

  async onTraceLoad(ctx: Trace): Promise<void> {
    await this.createBinderTransactionTrack(ctx, 'server', 'client');
    await this.createBinderTransactionTrack(ctx, 'client', 'server');
  }

  async createBinderTransactionTrack(
    ctx: Trace,
    perspective: string,
    oppositePerspective: string,
  ) {
    const binderCounterBreakdowns = new BreakdownTracks({
      trace: ctx,
      trackTitle: `Binder ${perspective} Transaction Counts`,
      modules: ['android.binder', 'android.binder_breakdown'],
      aggregationType: BreakdownTrackAggType.COUNT,
      aggregation: {
        columns: [
          `${perspective}_process`,
          `(IFNULL(interface, "unknown interface"))`,
          `(IFNULL(method_name, "unknown method"))`,
          `(${oppositePerspective}_process || ":" || ${oppositePerspective}_upid)`,
          `(${oppositePerspective}_thread || ":" ||  ${oppositePerspective}_utid)`,
        ],
        tsCol: `${oppositePerspective}_ts`,
        durCol: `${oppositePerspective}_dur`,
        tableName: 'android_binder_txns',
      },
      slice: {
        columns: ['aidl_name'],
        tableName: 'android_binder_txns',
        tsCol: `${oppositePerspective}_ts`,
        durCol: `${oppositePerspective}_dur`,
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

    ctx.defaultWorkspace.addChildInOrder(
      await binderCounterBreakdowns.createTracks(),
    );
  }
}
