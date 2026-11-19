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
  private videoFrame?: {readonly trackUri: string; readonly eventId: number};

  constructor(trace: Trace) {
    this.trace = trace;
  }

  async load(selection: TrackEventSelection): Promise<void> {
    this.videoFrame = undefined;
    const res = await this.trace.engine.query(`
      SELECT vf.id AS id
      FROM actual_frame_timeline_slice s
      JOIN __intrinsic_video_frames vf
        ON vf.frame_timeline_vsync_id = s.display_frame_token
      WHERE s.id = ${selection.eventId}
      ORDER BY vf.id
      LIMIT 1
    `);
    const row = res.maybeFirstRow({id: NUM});
    if (row === undefined) return;
    // Resolve through the VideoFrames track's dataset so its visibility rules
    // (no codec_config rows, nothing before the first key frame) and track URI
    // live in one place, and the link hides when the plugin is disabled.
    const matches = await this.trace.selection.resolveSqlEvents(
      '__intrinsic_video_frames',
      [row.id],
    );
    this.videoFrame = matches[0];
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
            this.trace.selection.selectTrackEvent(vf.trackUri, vf.eventId, {
              scrollToSelection: true,
            }),
        },
        'Jump to video frame',
      ),
    );
  }
}
