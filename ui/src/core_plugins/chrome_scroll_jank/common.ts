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

import {AddTrackArgs} from '../../common/actions';
import {ObjectByKey} from '../../common/state';
import {featureFlags} from '../../core/feature_flags';
import {CustomSqlDetailsPanelConfig} from '../../frontend/tracks/custom_sql_table_slice_track';

export const SCROLL_JANK_GROUP_ID = 'chrome-scroll-jank-track-group';

export const ENABLE_CHROME_SCROLL_JANK_PLUGIN = featureFlags.register({
  id: 'enableChromeScrollJankPlugin',
  name: 'Enable Chrome Scroll Jank plugin',
  description: 'Adds new tracks for scroll jank in Chrome',
  defaultValue: false,
});

export type DecideTracksResult = {
  tracksToAdd: AddTrackArgs[];
};

export interface ScrollJankTrackSpec {
  key: string;
  sqlTableName: string;
  detailsPanelConfig: CustomSqlDetailsPanelConfig;
}

// Global state for the scroll jank plugin.
export class ScrollJankPluginState {
  private static instance?: ScrollJankPluginState;
  private tracks: ObjectByKey<ScrollJankTrackSpec>;

  private constructor() {
    this.tracks = {};
  }

  public static getInstance(): ScrollJankPluginState {
    if (!ScrollJankPluginState.instance) {
      ScrollJankPluginState.instance = new ScrollJankPluginState();
    }

    return ScrollJankPluginState.instance;
  }

  public registerTrack(args: {
    kind: string;
    trackKey: string;
    tableName: string;
    detailsPanelConfig: CustomSqlDetailsPanelConfig;
  }): void {
    this.tracks[args.kind] = {
      key: args.trackKey,
      sqlTableName: args.tableName,
      detailsPanelConfig: args.detailsPanelConfig,
    };
  }

  public unregisterTrack(kind: string): void {
    delete this.tracks[kind];
  }

  public getTrack(kind: string): ScrollJankTrackSpec | undefined {
    return this.tracks[kind];
  }
}

export const ScrollJankV3TrackKind =
  'org.chromium.ScrollJank.scroll_jank_v3_track';

export const CHROME_EVENT_LATENCY_TRACK_KIND =
  'org.chromium.ScrollJank.event_latencies';
