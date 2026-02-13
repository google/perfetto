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

export type TrackSpec = {
  /**
   * Name of the table that contains events of a particular track. The primary
   * key of the table must be named 'id'.
   */
  tableName: string;

  /** The URI of the track in the Perfetto UI. */
  uri: string;

  /** Human-readable name of the track displayed in the Perfetto UI. */
  name: string;

  /**
   * Function which returns a correlated subquery which, given a column of an
   * outer SQL query which contains the ID of a scroll update event (aka
   * LatencyInfo.ID), returns the ID of the corresponding event on this track
   * (if available). This is to facilitate adding UI links between events on
   * this plugin's tracks.
   */
  eventIdSqlSubqueryForEventLatency: (eventLatencyIdColumn: string) => string;
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
  eventIdSqlSubqueryForEventLatency: (eventLatencyIdColumn: string) => `(
    SELECT ${EVENT_LATENCY_TRACK.tableName}.id
    FROM ${EVENT_LATENCY_TRACK.tableName}
    WHERE ${EVENT_LATENCY_TRACK.tableName}.scroll_update_id
        = ${eventLatencyIdColumn}
      AND ${EVENT_LATENCY_TRACK.tableName}.parent_id IS NULL
  )`,
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
  eventIdSqlSubqueryForEventLatency: (eventLatencyIdColumn: string) => `(
    SELECT ${SCROLL_TIMELINE_TRACK.tableName}.id
    FROM ${SCROLL_TIMELINE_TRACK.tableName}
    WHERE ${SCROLL_TIMELINE_TRACK.tableName}.scroll_update_id
        = ${eventLatencyIdColumn}
      AND ${SCROLL_TIMELINE_TRACK.tableName}.parent_id IS NULL
  )`,
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
  eventIdSqlSubqueryForEventLatency: (eventLatencyIdColumn: string) => `(
    SELECT ${SCROLL_TIMELINE_V4_TRACK.tableName}.id
    FROM ${SCROLL_TIMELINE_V4_TRACK.tableName}
    JOIN chrome_scroll_jank_v4_results
      ON ${SCROLL_TIMELINE_V4_TRACK.tableName}.original_slice_id
        = chrome_scroll_jank_v4_results.id
    WHERE chrome_scroll_jank_v4_results.first_event_latency_id
        = ${eventLatencyIdColumn}
      AND ${SCROLL_TIMELINE_V4_TRACK.tableName}.parent_id IS NULL
  )`,
};
