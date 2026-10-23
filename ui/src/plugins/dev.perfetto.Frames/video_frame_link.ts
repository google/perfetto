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
  private videoFrame?: {readonly id: number; readonly displayId: number};

  constructor(trace: Trace) {
    this.trace = trace;
  }

  async load(selection: TrackEventSelection): Promise<void> {
    this.videoFrame = undefined;
    // Only frames the video track shows (see createVideoFramesTrack): no
    // codec_config rows, and nothing before the display's first key frame.
    const res = await this.trace.engine.query(`
      SELECT vf.id AS id, vf.display_id AS displayId
      FROM actual_frame_timeline_slice s
      JOIN __intrinsic_video_frames vf
        ON vf.frame_timeline_vsync_id = s.display_frame_token
      WHERE s.id = ${selection.eventId}
        AND COALESCE(vf.is_config, 0) = 0
        AND vf.ts >= (
          SELECT MIN(ts) FROM __intrinsic_video_frames
          WHERE display_id = vf.display_id AND is_key_frame = 1
        )
      ORDER BY vf.id
      LIMIT 1
    `);
    const row = res.maybeFirstRow({id: NUM, displayId: NUM});
    if (row === undefined) return;
    this.videoFrame = {id: row.id, displayId: row.displayId};
  }

  render(): m.Children {
    const vf = this.videoFrame;
    if (vf === undefined) return undefined;
    const trackUri = `/video_frames/${vf.displayId}`;
    // The track is registered by the VideoFrames plugin, which may be off.
    if (this.trace.tracks.getTrack(trackUri) === undefined) return undefined;
    return m(
      Section,
      {title: 'Display video'},
      m(
        Anchor,
        {
          icon: 'movie',
          onclick: () =>
            this.trace.selection.selectTrackEvent(trackUri, vf.id, {
              scrollToSelection: true,
            }),
        },
        'Jump to video frame',
      ),
    );
  }
}
