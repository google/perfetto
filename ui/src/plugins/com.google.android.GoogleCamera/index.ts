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

import type {Trace} from '../../public/trace';
import type {PerfettoPlugin} from '../../public/plugin';
import {addDebugCounterTrack} from '../../components/tracks/debug_tracks';
import * as cameraConstants from './googleCameraConstants';

export default class implements PerfettoPlugin {
  static readonly id = 'com.google.android.GoogleCamera';
  async onTraceLoad(ctx: Trace): Promise<void> {
    ctx.commands.registerCommand({
      id: 'com.google.android.LoadGoogleCameraStartupView',
      name: 'Load google camera startup view',
      callback: () => {
        this.loadGCAStartupView(ctx);
      },
    });

    ctx.commands.registerCommand({
      id: 'com.google.android.PinCameraRelatedTracks',
      name: 'Pin camera related tracks',
      callback: async () => {
        const promptResult = await ctx.omnibox.prompt(
          'List of additional track names that you would like to pin separated by commas',
        );
        const rawTrackNames = promptResult ?? '';
        const trackNameList = rawTrackNames
          .split(',')
          .map((item) => item.trim());
        this.pinTracks(ctx, trackNameList);
      },
    });

    ctx.commands.registerCommand({
      id: 'com.google.android.AddGoogleCameraMemoryTracks',
      name: 'Add GoogleCamera memory tracks',
      callback: () => {
        this.addGoogleCameraMemoryTracks(ctx);
      },
    });
  }

  private loadGCAStartupView(ctx: Trace) {
    this.pinTracks(ctx, cameraConstants.MAIN_THREAD_TRACK);
    this.pinTracks(ctx, cameraConstants.STARTUP_RELATED_TRACKS);
  }

  private pinTracks(ctx: Trace, trackNames: ReadonlyArray<string>) {
    ctx.currentWorkspace.flatTracks.forEach((track) => {
      trackNames.forEach((trackName) => {
        if (track.name.match(trackName)) {
          track.pin();
        }
      });
    });
  }

  private async addGoogleCameraMemoryTracks(ctx: Trace) {
    await ctx.engine.query('INCLUDE PERFETTO MODULE pixel.camera;');

    const memoryTracks = [
      {
        col: 'gca_rss',
        name: 'GoogleCamera RSS',
      },
      {
        col: 'hal_rss',
        name: 'Camera HAL RSS',
      },
      {
        col: 'cameraserver_rss',
        name: 'CameraServer RSS',
      },
      {
        col: 'dma',
        name: 'DMABUF',
      },
      {
        col: 'rss_and_dma',
        name: 'Total Camera RSS + DMABUF',
      },
    ] as const;

    for (const t of memoryTracks) {
      await addDebugCounterTrack({
        trace: ctx,
        data: {
          sqlSource: `SELECT ts, ${t.col} AS value FROM pixel_camera_memory_span`,
          columns: ['ts', 'value'],
        },
        title: t.name,
      });
    }
  }
}
