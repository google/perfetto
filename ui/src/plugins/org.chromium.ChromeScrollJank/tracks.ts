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

type TrackSpec = {
  tableName: string;
  uri: string;
  name: string;
};

/**
 * The EventLatency track.
 *
 * The track contains information about scroll updates and their stages, which
 * can be visualized by {@link event_latency_track#createEventLatencyTrack} and
 * whose details can be inspected in
 * {@link event_latency_details_panel#EventLatencySliceDetailsPanel}.
 *
 * The undeflying table is created by
 * {@link index#default.addEventLatencyTrack}.
 *
 * EventLatency slices on the track are marked as janky/non-janky based on the
 * Perfetto scroll_jank_v3 metric. See the `chrome_janky_event_latencies_v3`
 * table for more details.
 */
export const EVENT_LATENCY_TRACK: TrackSpec = {
  tableName: `org_chromium_ChromeScrollJank_event_latency`,
  uri: 'org.chromium.ChromeScrollJank#eventLatency',
  name: 'Chrome Scroll Input Latencies',
};

/**
 * The scroll timeline track.
 *
 * The timeline and the underlying table contain information about scroll
 * updates and their stages, which can be visualized by
 * {@link scroll_timeline_track#createScrollTimelineTrack} and whose details can
 * be inspected in
 * {@link scroll_timeline_details_panel#ScrollTimelineDetailsPanel}.
 *
 * The underlying table is created by
 * {@link scroll_timeline_model#createScrollTimelineModel}. See
 * {@link scroll_timeline_model#SCROLL_TIMELINE_TABLE_DEFINITION} for more
 * information about the table's contents.
 *
 * Scroll updates on the scroll timeline are marked as janky/non-janky based on
 * Chrome's v3.1 scroll jank metric
 * ('Event.ScrollJank.DelayedFramesPercentage.FixedWindow'). See
 * https://source.chromium.org/chromium/chromium/src/+/main:cc/metrics/scroll_jank_dropped_frame_tracker.h
 * for more details.
 *
 * In general, there's a 1:1 mapping between scroll updates on this track and
 * EventLatency slices on {@link EVENT_LATENCY_TRACK}.
 */
export const SCROLL_TIMELINE_TRACK: TrackSpec = {
  tableName: 'org_chromium_ChromeScrollJank_scroll_timeline',
  uri: 'org.chromium.ChromeScrollJank#scrollTimeline',
  name: 'Chrome Scroll Timeline',
};

/**
 * The scroll timeline track according to Chrome's scroll jank v4 metric.
 *
 * The timeline and the underlying table contain information about frames which
 * contain one or more scroll updates and relevant events within the frames,
 * which can be visualized by
 * {@link scroll_timeline_v4_track#createScrollTimelineV4Track}.
 *
 * The underlying table is created by
 * {@link scroll_timeline_v4_model#createScrollTimelineV4Model}. See
 * {@link scroll_timeline_v4_model#SCROLL_TIMELINE_V4_TABLE_DEFINITION} for more
 * information about the table's contents.
 *
 * Frames on the scroll timeline v4 are marked as janky/non-janky based on
 * Chrome's v4 scroll jank metric
 * ('Event.ScrollJank.DelayedFramesPercentage4.FixedWindow'). See
 * https://docs.google.com/document/d/1AaBvTIf8i-c-WTKkjaL4vyhQMkSdynxo3XEiwpofdeA
 * and scroll_jank_v4*.{h,cc} source files in
 * https://source.chromium.org/chromium/chromium/src/+/main:cc/metrics/ for more
 * details.
 *
 * In general, there's a 1:N mapping (N >= 1) between frames on this track and
 * scroll updates on {@link SCROLL_TIMELINE_TRACK}.
 */
export const SCROLL_TIMELINE_V4_TRACK: TrackSpec = {
  tableName: 'org_chromium_ChromeScrollJank_scroll_timeline_v4',
  uri: 'org.chromium.ChromeScrollJank#scrollTimelineV4',
  name: 'Chrome Scroll Timeline v4',
};
