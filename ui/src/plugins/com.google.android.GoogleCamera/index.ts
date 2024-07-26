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

import {
  Plugin,
  PluginContextTrace,
  PluginDescriptor,
  TrackRef,
} from '../../public';

import * as cameraConstants from './googleCameraConstants';

class GoogleCamera implements Plugin {
  private ctx!: PluginContextTrace;

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    this.ctx = ctx;

    ctx.registerCommand({
      id: 'com.google.android.GoogleCamera#LoadGoogleCameraStartupView',
      name: 'Load google camera startup view',
      callback: () => {
        this.loadGCAStartupView();
      },
    });

    ctx.registerCommand({
      id: 'com.google.android.GoogleCamera#PinCameraRelatedTracks',
      name: 'Pin camera related tracks',
      callback: (trackNames) => {
        trackNames = prompt(
          'List of additional track names that you would like to pin separated by commas',
          '',
        );
        const trackNameList = trackNames.split(',').map(function (
          item: string,
        ) {
          return item.trim();
        });
        this.pinTracks(trackNameList);
      },
    });
  }

  private loadGCAStartupView() {
    this.pinTracks(cameraConstants.MAIN_THREAD_TRACK);
    this.pinTracks(cameraConstants.STARTUP_RELATED_TRACKS);
  }

  private pinTracks(trackNames: string[]) {
    const tracks: TrackRef[] = this.ctx.timeline.tracks;
    trackNames.forEach((trackName) => {
      const desiredTracks = tracks.filter((track) => {
        return track.title.match(trackName);
      });
      desiredTracks.forEach((desiredTrack) => {
        this.ctx.timeline.pinTrack(desiredTrack.key!);
      });
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'com.google.android.GoogleCamera',
  plugin: GoogleCamera,
};
