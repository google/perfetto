// Copyright (C) 2025 The Android Open Source Project
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

import type {
  NotificationsBlockingCallPinRequest,
  PinRequest,
} from './pinRequest';
import {PinRequestType} from './pinRequest';
import type {Trace} from '../../../public/trace';
import {addDebugSliceTrack} from '../../../components/tracks/debug_tracks';

/**
 * Translates a notifications blocking call metric key into a request to pin the
 * notifications blocking-call track.
 *
 * @param {string} metricKey The metric key to match.
 * @returns {PinRequest[]} A single NotificationsBlockingCall request, or [] if
 *     the key doesn't match.
 */
export function translateNotificationsBlockingCall(
  metricKey: string,
): PinRequest[] {
  const matcher =
    /perfetto_android_notifications_blocking_call-blocking_calls-name-(?<blockingCallName>([^\-]*))-(?<aggregation>.*)/;
  const match = matcher.exec(metricKey);
  if (!match?.groups) {
    return [];
  }
  return [
    {
      type: PinRequestType.NotificationsBlockingCall,
      notificationName: match.groups.blockingCallName,
      aggregation: match.groups.aggregation,
    },
  ];
}

/**
 * Adds the debug tracks for Notifications Blocking Call metrics.
 *
 * @param {Trace} ctx PluginContextTrace for trace related properties and methods
 * @param {NotificationsBlockingCallPinRequest} req The notification to pin.
 * @returns {void} Adds one track for Notifications Blocking Call slice of metric
 */
export function execNotificationsBlockingCall(
  ctx: Trace,
  req: NotificationsBlockingCallPinRequest,
): Promise<void> {
  const config = notificationsBlockingCallTrackConfig(req);
  addDebugSliceTrack({trace: ctx, ...config});
  return Promise.resolve();
}

function notificationsBlockingCallTrackConfig(
  req: NotificationsBlockingCallPinRequest,
) {
  const notificationName = req.notificationName;

  // Avoid use of android_sysui_notifications_blocking_calls_metric.sql, in favour of stdlib migration
  // The query below is derived from android_sysui_notifications_blocking_calls_metric.sql
  // See table "android_sysui_notifications_blocking_calls"
  const notificationsBlockingCallsQuery = `
SELECT
    s.name name,
    s.ts ts,
    s.dur dur
FROM slice s
    JOIN thread_track ON s.track_id = thread_track.id
    JOIN thread USING (utid)
WHERE
    thread.is_main_thread AND
    _is_relevant_notifications_blocking_call(s.name, s.dur)
  `;

  const trackName = notificationName + ' blocking calls';
  return {
    data: {
      sqlSource: notificationsBlockingCallsQuery,
      columns: ['name', 'ts', 'dur'],
    },
    columns: {ts: 'ts', dur: 'dur', name: 'name'},
    argColumns: ['name', 'ts', 'dur'],
    title: trackName,
  };
}
