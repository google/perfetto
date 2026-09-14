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

import m from 'mithril';
import {Anchor} from '../../widgets/anchor';
import {Section} from '../../widgets/section';
import {NUM} from '../../trace_processor/query_result';
import type {Trace} from '../../public/trace';
import type {TrackEventSelection} from '../../public/selection';
import type {TrackEventDetailsPanelSection} from '../../components/details/thread_slice_details_tab';

// Details-panel section for an actual frame-timeline slice - both the
// SurfaceFlinger DisplayFrame composite and the app SurfaceFrames under it -
// linking to the captured display-video frame that composite produced, when
// the trace has one. Both slice kinds carry the same DisplayFrame token, so a
// single join on frame_timeline_vsync_id covers both. Renders nothing when
// there's no matching frame, so it self-hides on traces without video.
export class VideoFrameLinkSection implements TrackEventDetailsPanelSection {
  private readonly trace: Trace;
  private videoFrame?: {id: number; displayId: number};

  constructor(trace: Trace) {
    this.trace = trace;
  }

  async load(selection: TrackEventSelection): Promise<void> {
    this.videoFrame = undefined;
    try {
      const res = await this.trace.engine.query(`
        SELECT vf.id AS id, vf.display_id AS displayId
        FROM actual_frame_timeline_slice s
        JOIN __intrinsic_video_frames vf
          ON vf.frame_timeline_vsync_id = s.display_frame_token
        WHERE s.id = ${selection.eventId}
        LIMIT 1
      `);
      if (res.numRows() === 0) return;
      const row = res.firstRow({id: NUM, displayId: NUM});
      this.videoFrame = {id: row.id, displayId: row.displayId};
    } catch {
      // No video-frame table in this build; leave the section hidden.
    }
  }

  render(): m.Children {
    const vf = this.videoFrame;
    if (vf === undefined) return undefined;
    return m(
      Section,
      {title: 'Display video'},
      m(
        Anchor,
        {
          icon: 'movie',
          onclick: () =>
            this.trace.selection.selectTrackEvent(
              `/video_frames/${vf.displayId}`,
              vf.id,
              {scrollToSelection: true},
            ),
        },
        'Jump to video frame',
      ),
    );
  }
}
