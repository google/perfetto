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

import {NUM} from '../../../trace_processor/query_result';
import type {Trace} from '../../../public/trace';
import {
  expandProcessName,
  type MetricHandler,
  type ProcessMetricData,
} from './metricUtils';

export class SimpleProcessMetricHandler implements MetricHandler {
  /**
   * Base class for simple logic track pinning
   * Use when you have a Regexp which can extract a process name from an url
   * And pin tracks in the found process
   *
   * @param {RegExp[]} matchers List of matchers to check original url with
   * @param {string[]} trackMatchers Matches track in the process for pinning (using prefix match)
   */
  constructor(
    private readonly matchers: RegExp[],
    private readonly trackMatchers: string[],
  ) {}

  /**
   * Matches metric key & return parsed data if successful.
   *
   * @param {string} metricKey The metric key to match.
   * @returns {ProcessMetricData | undefined} Parsed data or undefined if no match.
   */
  public match(metricKey: string): ProcessMetricData | undefined {
    for (const matcher of this.matchers) {
      const match = matcher.exec(metricKey);
      if (match?.groups?.processName) {
        return {
          process: expandProcessName(match.groups.processName),
        };
      }
    }
    return undefined;
  }

  /**
   * Pins matching tracks for the specified process.
   *
   * @param {ProcessMetricData} metricData Parsed metric data.
   * @param {Trace} ctx Trace context.
   */
  public async addMetricTrack(metricData: ProcessMetricData, ctx: Trace) {
    const processName = metricData.process;
    const upid = await getUpidForProcess(ctx, processName);
    if (upid === undefined) {
      return;
    }

    // Find the tracks to pin.
    const tracksToPin = ctx.currentWorkspace.flatTracks.filter((track) => {
      const name = track.name;
      const isTargetTrack = this.trackMatchers.some((matcher) => {
        return name.startsWith(matcher);
      });

      if (!isTargetTrack) {
        return false;
      }
      if (!track.uri) {
        return false;
      }
      const descriptor = ctx.tracks.getTrack(track.uri);
      return descriptor?.tags?.upid === upid;
    });

    if (tracksToPin.length > 0) {
      tracksToPin.forEach((track) => {
        track.pin();
      });
    }
  }
}

async function getUpidForProcess(
  ctx: Trace,
  processName: string,
): Promise<number | undefined> {
  const query = `
    INCLUDE PERFETTO MODULE android.process_metadata;
    select
      _process_available_info_summary.upid
    from _process_available_info_summary
    join process using(upid)
    where process.name = '${processName}'
    limit 1;
  `;
  const res = await ctx.engine.query(query);

  if (res.numRows() === 0) {
    return undefined;
  }
  return res.firstRow({upid: NUM}).upid;
}
