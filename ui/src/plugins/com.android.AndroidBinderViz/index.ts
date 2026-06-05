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
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import type {TrackNode} from '../../public/workspace';
import {BinderSliceDetailsPanel} from './details_panel';

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidBinderViz';

  async onTraceLoad(ctx: Trace): Promise<void> {
    const [serverRoot, clientRoot] = await Promise.all([
      this.createBinderTransactionTrack(
        ctx,
        'server',
        'client',
        'binder_txn_id',
      ),
      this.createBinderTransactionTrack(
        ctx,
        'client',
        'server',
        'binder_reply_id',
      ),
    ]);
    ctx.defaultWorkspace.addChildInOrder(serverRoot);
    ctx.defaultWorkspace.addChildInOrder(clientRoot);
  }

  async createBinderTransactionTrack(
    ctx: Trace,
    perspective: string,
    oppositePerspective: string,
    sliceIdColumn?: string,
  ): Promise<TrackNode> {
    const binderCounterBreakdowns = new BreakdownTracks({
      trace: ctx,
      trackTitle: `Binder ${perspective} Transaction Counts`,
      modules: ['android.binder'],
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
        columns: ['IFNULL(aidl_name, "unknown aidl")'],
        tableName: 'android_binder_txns',
        tsCol: `${oppositePerspective}_ts`,
        durCol: `${oppositePerspective}_dur`,
      },
      sliceIdColumn: sliceIdColumn,
      sortTracks: false,
      detailsPanel: (trace: Trace) => new BinderSliceDetailsPanel(trace),
    });

    return await binderCounterBreakdowns.createTracks();
  }
}
