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
import {QuerySlot} from '../../base/query_slot';
import {materialColorScheme} from '../../components/colorizer';
import {SliceTrack} from '../../components/tracks/slice_track';
import type {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {VideoFrameDetailsPanel} from './video_frame_details_panel';
import type {VideoFramePlayer} from './video_frame_player';

export function createVideoFramesTrack(
  trace: Trace,
  uri: string,
  displayId: number,
  player: VideoFramePlayer,
) {
  // Each access unit is an instant on the timeline, but -- like the
  // Screenshots track -- we want each frame's slice to visibly cover the
  // span until the next frame, with the same hover-to-preview UX.
  //
  // Setting dur = -1 and depth = 0 renders them as incomplete slices: a
  // rect from the frame's ts up to the following frame's ts, fading out to
  // the right. The fade conveys that the painted frame is exact at the
  // slice start and merely held until the next on-screen change.
  //
  // is_config rows are decoder-setup pseudo-frames, not displayable
  // frames, so they're excluded. The per-frame name doubles as the
  // colorization seed, so adjacent frames get contrasting colors.
  const src = `
    SELECT
      id,
      ts,
      -1 AS dur,
      0 AS depth,
      'Frame ' || frame_number AS name
    FROM android_video_frames
    WHERE display_id = ${displayId}
      AND COALESCE(is_config, 0) = 0
  `;

  // Hover preview: decode the hovered frame to a PNG data URL via the
  // player's read-only path (no effect on the live preview canvas or
  // playback) and show it in the tooltip. QuerySlot caches per-id so
  // moving the cursor along the track is seamless and re-hovering a frame
  // is instant.
  const imageSlot = new QuerySlot<string | undefined>();

  // Singleton panel: returning the same instance on every selection lets
  // mithril patch the DOM in place rather than remount the canvas. Without
  // this, every selectTrackEvent from the play loop unmounts the canvas
  // (-> detachCanvas -> stop()) and kills playback after one frame, plus
  // causes visible flicker on every selection change.
  const panel = new VideoFrameDetailsPanel(player);

  return SliceTrack.create({
    trace,
    uri,
    dataset: new SourceDataset({
      schema: {id: NUM, ts: LONG, dur: LONG, name: STR, depth: NUM},
      src,
    }),
    detailsPanel: () => panel,
    colorizer: (row) => materialColorScheme(row.name),
    tooltip: (data) => {
      const image = imageSlot.use({
        key: {id: data.id},
        // Show the previously decoded frame while the next one decodes
        // (retainOn keeps stale data across id changes), so sweeping the
        // cursor along the track never flips back to 'Loading...' and the
        // preview never blinks -- the same smoothness the Screenshots
        // track gets for free from its instant arg lookup.
        retainOn: ['id'],
        queryFn: () => player.decodeFrameImage(data.id),
      });
      // Show the pure decoded image, sized to itself (capped), exactly
      // like the Screenshots track -- no wrapper box, so there is no
      // letterbox / white padding around the frame. While the first frame
      // is still decoding we have no image to show; the previous frame is
      // retained (retainOn) so this only matters on the very first hover,
      // where we briefly show the 'Loading...' text.
      if (image.data) {
        return [m('img.pf-video-frame-tooltip__img', {src: image.data})];
      }
      return 'Loading...';
    },
  });
}
