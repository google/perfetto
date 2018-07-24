
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

import {createPage} from './pages';
import {ScrollingTrackDisplay} from './scrolling_track_display';
import {TimeScale} from './time_scale';

/**
 * Top-most level component for the viewer page. Holds tracks, brush timeline,
 * panels, and everything else that's part of the main trace viewer page.
 */
const TraceViewer = {
  view() {
    const timeScale = new TimeScale([0, 1000000], [0, 1000]);
    return m(ScrollingTrackDisplay, {
      timeScale,
    });
  },
} as m.Component<{}, {}>;

export const ViewerPage = createPage({
  view() {
    return m(TraceViewer);
  }
});