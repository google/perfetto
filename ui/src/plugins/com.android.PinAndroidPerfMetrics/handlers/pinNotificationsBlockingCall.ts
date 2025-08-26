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

import {
  NotificationsBlockingCallMetricData,
  MetricHandler,
} from './metricUtils';
import {Trace} from '../../../public/trace';
import {addDebugSliceTrack} from '../../../components/tracks/debug_tracks';

class BlockingCallMetricHandler implements MetricHandler {
  /**
   * Matches metric key for notifications blocking call metrics & return parsed data if successful.
   *
   * @param {string} metricKey The metric key to match.
   * @returns {NotificationsBlockingCallMetricData | undefined} Parsed data or undefined if no match.
   */
  public match(
    metricKey: string,
  ): NotificationsBlockingCallMetricData | undefined {
    const matcher =
      /perfetto_android_notifications_blocking_call-blocking_calls-name-(?<blockingCallName>([^\-]*))-(?<aggregation>.*)/;
    const match = matcher.exec(metricKey);
    if (!match?.groups) {
      return undefined;
    }
    const metricData: NotificationsBlockingCallMetricData = {
      notificationName: match.groups.blockingCallName,
      aggregation: match.groups.aggregation,
    };
    return metricData;
  }

  /**
   * Adds the debug tracks for Notifications Blocking Call metrics
   *
   * @param {NotificationsBlockingCallMetricData} metricData Parsed metric data
   * @param {Trace} ctx PluginContextTrace for trace related properties and methods
   * @returns {void} Adds one track for Notifications Blocking Call slice of metric
   */
  public addMetricTrack(
    metricData: NotificationsBlockingCallMetricData,
    ctx: Trace,
  ): void {
    const config = this.notificationsBlockingCallTrackConfig(metricData);
    addDebugSliceTrack({trace: ctx, ...config});
  }

  private notificationsBlockingCallTrackConfig(
    metricData: NotificationsBlockingCallMetricData,
  ) {
    const notificationName = metricData.notificationName;

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
}

export const pinNotificationsBlockingCallHandlerInstance =
  new BlockingCallMetricHandler();
