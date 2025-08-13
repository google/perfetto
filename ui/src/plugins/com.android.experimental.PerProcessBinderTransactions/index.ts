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

import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {createQuerySliceTrack} from '../../components/tracks/query_slice_track';
import {TrackNode} from '../../public/workspace';

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.experimental.PerProcessBinderTransactions';

  async onTraceLoad(ctx: Trace): Promise<void> {
    ctx.commands.registerCommand({
      id: 'com.android.experimental.PerProcessBinderTransactions#ShowProcessBinderTransactions',
      name: 'Add track: show binder server slices for a client PID (ex system_server, SurfaceFlinger)',
      callback: async (pid) => {
        if (pid === undefined) {
          pid = prompt('Enter a client process pid', '');
          if (pid === null) return;
        }

        await ctx.engine.query(`INCLUDE PERFETTO MODULE android.binder;`);

        const trackAppUri = `/app_binder_slices_for_${pid}`;
        const trackAppName = `App Binders for PID ${pid}`;
        const trackNativeUri = `/native_binder_slices_for_${pid}`;
        const trackNativeName = `Native Binders for PID ${pid}`;

        const appTrack = await createQuerySliceTrack({
          trace: ctx,
          uri: trackAppUri,
          data: {
            sqlSource: `
              SELECT
                server_ts AS ts,
                server_dur AS dur,
                server_process AS name
              FROM android_binder_txns
              WHERE client_pid = ${pid}
              AND server_package_version_code IS NOT NULL
            `,
            columns: ['ts', 'dur', 'name'],
          },
        });
        // Native binder track excludes System Server and SurfaceFlinger
        // as they trigger very frequently and make the Native track too noisy
        const nativeTrack = await createQuerySliceTrack({
          trace: ctx,
          uri: trackNativeUri,
          data: {
            sqlSource: `
              SELECT
                server_ts AS ts,
                server_dur AS dur,
                server_process AS name
              FROM android_binder_txns
              WHERE client_pid = ${pid}
              AND server_package_version_code IS NULL
              AND server_process != "system_server"
              AND server_process != "/system/bin/surfaceflinger"
            `,
            columns: ['ts', 'dur', 'name'],
          },
        });

        ctx.tracks.registerTrack({
          uri: trackAppUri,
          renderer: appTrack,
        });
        ctx.tracks.registerTrack({
          uri: trackNativeUri,
          renderer: nativeTrack,
        });

        const trackNodeApp = new TrackNode({
          name: trackAppName,
          uri: trackAppUri,
        });
        const trackNodeNonApp = new TrackNode({
          name: trackNativeName ,
          uri: trackNativeUri,
        });

        ctx.workspace.addChildInOrder(trackNodeApp);
        ctx.workspace.addChildInOrder(trackNodeNonApp);
      },
    });
  }
}
