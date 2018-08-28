// Copyright (C) 2018 The Android Open Source Project
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

import * as m from 'mithril';

import {TrackState} from '../common/state';

import {FlameGraphPanel} from './flame_graph_panel';
import {globals} from './globals';
import {HeaderPanel} from './header_panel';
import {Panel} from './panel';
import {PanelContainer} from './panel_container';
import {TrackPanel} from './track_panel';

class TrackPanelById {
  private idToTrackPanel: Map<string, TrackPanel>;
  constructor() {
    this.idToTrackPanel = new Map();
  }

  getOrCreateTrack(trackState: TrackState): TrackPanel {
    let trackPanel = this.idToTrackPanel.get(trackState.id);
    if (trackPanel === undefined) {
      trackPanel = new TrackPanel(trackState);
      this.idToTrackPanel.set(trackState.id, trackPanel);
    }
    return trackPanel;
  }

  clearObsoleteTracks(currentTrackIds: string[]) {
    const currentTrackIdSet = new Set(currentTrackIds);
    for (const id of this.idToTrackPanel.keys()) {
      if (!currentTrackIdSet.has(id)) {
        this.idToTrackPanel.delete(id);
      }
    }
  }

  clearAll() {
    this.idToTrackPanel.clear();
  }
}

interface ScrollingPanelContainerState {
  trackHeaderPanel?: Panel;
  flameGraphPanel?: Panel;
  trackPanelById: TrackPanelById;
}

export const ScrollingPanelContainer = {
  oninit() {
    this.trackPanelById = new TrackPanelById();
  },

  view() {
    if (globals.state.displayedTrackIds.length === 0) {
      this.trackHeaderPanel = undefined;
      this.flameGraphPanel = undefined;
      this.trackPanelById.clearAll();
      return m('.scrolling-panel-container');
    }

    const panels: Panel[] = [];

    // The header can't be part of the track panel right now because each track
    // is its own panel but we want only one header.
    // TODO: Move this to TrackCollection panel when it is introduced.
    if (this.trackHeaderPanel === undefined) {
      this.trackHeaderPanel = new HeaderPanel('Tracks');
    }
    panels.push(this.trackHeaderPanel);

    const displayedTrackIds = globals.state.displayedTrackIds;
    this.trackPanelById.clearObsoleteTracks(displayedTrackIds);
    for (const id of displayedTrackIds) {
      const trackState = globals.state.tracks[id];
      const trackPanel = this.trackPanelById.getOrCreateTrack(trackState);
      panels.push(trackPanel);
    }

    if (this.flameGraphPanel === undefined) {
      this.flameGraphPanel = new FlameGraphPanel();
    }
    panels.push(this.flameGraphPanel);

    return m(
        '.scrolling-panel-container',
        m(PanelContainer, {panels, doesScroll: true}));
  },
} as m.Component<{}, ScrollingPanelContainerState>;
