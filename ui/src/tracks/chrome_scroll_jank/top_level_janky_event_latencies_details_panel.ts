// Copyright (C) 2023 The Android Open Source Project
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

import {exists} from '../../base/utils';
import {EngineProxy} from '../../common/engine';
import {LONG, NUM, STR, STR_NULL} from '../../common/query_result';
import {
  TPDuration,
  tpDurationFromSql,
  TPTime,
  tpTimeFromSql,
} from '../../common/time';
import {
  BottomTab,
  bottomTabRegistry,
  NewBottomTabArgs,
} from '../../frontend/bottom_tab';
import {
  GenericSliceDetailsTabConfig,
} from '../../frontend/generic_slice_details_tab';
import {getSlice, SliceDetails, sliceRef} from '../../frontend/sql/slice';
import {asSliceSqlId, asTPTimestamp} from '../../frontend/sql_types';
import {sqlValueToString} from '../../frontend/sql_utils';
import {DetailsShell} from '../../frontend/widgets/details_shell';
import {Duration} from '../../frontend/widgets/duration';
import {GridLayout} from '../../frontend/widgets/grid_layout';
import {Section} from '../../frontend/widgets/section';
import {SqlRef} from '../../frontend/widgets/sql_ref';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {dictToTreeNodes, Tree, TreeNode} from '../../frontend/widgets/tree';

import {
  eventLatencySlice,
  EventLatencySlice,
  getEventLatencyDescendantSlice,
  getEventLatencySlice,
} from './event_latency_slice';
import {ScrollJankPluginState} from './index';
import {
  TopLevelEventLatencyTrack,
} from './top_level_janky_event_latencies_track';
import {raf} from '../../core/raf_scheduler';

interface Data {
  id: number;
  // The slice display name - the cause + subcause of jank.
  name: string;
  // The stage of EventLatency that is the cause of jank.
  jankCause: string;
  // Where possible, the subcause of jank.
  jankSubcause: string;
  // The slice type - e.g. EventLatency
  type: string;
  // Timestamp of the beginning of this slice in nanoseconds.
  ts: TPTime;
  // Duration of this slice in nanoseconds.
  dur: TPDuration;
}

async function getSliceDetails(
    engine: EngineProxy, id: number): Promise<SliceDetails|undefined> {
  return getSlice(engine, asSliceSqlId(id));
}

export class JankyEventLatenciesDetailsPanel extends
    BottomTab<GenericSliceDetailsTabConfig> {
  static readonly kind = 'org.perfetto.JankyEventLatenciesDetailsPanel';
  static title = 'Chrome Scroll Jank Causes';
  private loaded = false;
  private sliceDetails?: SliceDetails;
  private eventLatencySliceDetails?: EventLatencySlice;
  private causeSliceDetails?: EventLatencySlice;
  private subcauseSliceDetails?: EventLatencySlice;

  data: Data|undefined;

  static create(args: NewBottomTabArgs): JankyEventLatenciesDetailsPanel {
    return new JankyEventLatenciesDetailsPanel(args);
  }

  constructor(args: NewBottomTabArgs) {
    super(args);
    this.loadData();
  }

  private async loadData() {
    const trackDetails = ScrollJankPluginState.getInstance().getTrack(
        TopLevelEventLatencyTrack.kind);

    const queryResult = await this.engine.query(`
        SELECT
          id,
          name,
          jank_cause AS jankCause,
          jank_subcause AS jankSubcause,
          type,
          ts,
          dur
        FROM ${trackDetails?.sqlTableName} where id = ${this.config.id}`);

    const iter = queryResult.firstRow({
      id: NUM,
      name: STR,
      jankCause: STR,
      jankSubcause: STR_NULL,
      type: STR,
      ts: LONG,
      dur: LONG,
    });
    this.data = {
      id: iter.id,
      name: iter.name,
      jankCause: iter.jankCause,
      jankSubcause: iter.jankSubcause,
      type: iter.type,
      ts: iter.ts,
      dur: iter.dur,
    } as Data;

    await this.loadSlices();
    this.loaded = true;

    raf.scheduleFullRedraw();
  }

  private hasCause(): boolean {
    if (this.data === undefined) {
      return false;
    }
    return this.data.jankCause !== 'UNKNOWN';
  }

  private hasSubcause(): boolean {
    return this.hasCause() && this.data?.jankSubcause !== undefined;
  }

  private async loadSlices() {
    this.sliceDetails = await getSliceDetails(this.engine, this.config.id);
    this.eventLatencySliceDetails =
        await getEventLatencySlice(this.engine, this.config.id);

    if (this.hasCause()) {
      this.causeSliceDetails = await getEventLatencyDescendantSlice(
          this.engine, this.config.id, this.data?.jankCause);
    }

    if (this.hasSubcause()) {
      this.subcauseSliceDetails = await getEventLatencyDescendantSlice(
          this.engine, this.config.id, this.data?.jankSubcause);
    }
  }

  viewTab() {
    if (this.data === undefined) {
      return m('h2', 'Loading');
    }

    const detailsDict: {[key: string]: m.Child} = {
      'Janked Event Latency stage':
          exists(this.sliceDetails) && exists(this.causeSliceDetails) ?
          eventLatencySlice(
              this.causeSliceDetails,
              this.data.jankCause,
              this.sliceDetails.sqlTrackId) :
          sqlValueToString(this.data.jankCause),
    };

    if (sqlValueToString(this.data.jankSubcause) != 'NULL') {
      detailsDict['Sub-cause of Jank'] =
          exists(this.sliceDetails) && exists(this.subcauseSliceDetails) ?
          eventLatencySlice(
              this.subcauseSliceDetails,
              this.data.jankSubcause,
              this.sliceDetails.sqlTrackId) :
          sqlValueToString(this.data.jankSubcause);
    }

    detailsDict['Start time'] =
        m(Timestamp, {ts: asTPTimestamp(tpTimeFromSql(this.data.ts))});
    detailsDict['Duration'] =
        m(Duration, {dur: tpDurationFromSql(this.data.dur)});
    detailsDict['Slice Type'] = sqlValueToString(this.data.type as string);

    const details = dictToTreeNodes(detailsDict);

    if (exists(this.sliceDetails)) {
      details.push(m(TreeNode, {
        left: sliceRef(this.sliceDetails, 'Original EventLatency'),
        right: '',
      }));
      if (exists(this.eventLatencySliceDetails)) {
        details.push(m(TreeNode, {
          left: eventLatencySlice(
              this.eventLatencySliceDetails,
              'Chrome Input Event Latencies',
              this.sliceDetails.sqlTrackId),
          right: '',
        }));
      }
    }

    // TODO(b/278844325): add links to the correct process/track for cause.

    return m(
        DetailsShell,
        {
          title: JankyEventLatenciesDetailsPanel.title,
        },
        m(
            GridLayout,
            m(
                Section,
                {title: 'Details'},
                m(Tree, details),
                ),
            m(
                Section,
                {title: 'Metadata'},
                m(Tree, [m(TreeNode, {
                    left: 'SQL ID',
                    right: m(SqlRef, {
                      table: 'chrome_janky_event_latencies_v3',
                      id: this.config.id,
                    }),
                  })]),
                ),
            ),
    );
  }

  getTitle(): string {
    return `Current Selection`;
  }

  isLoading() {
    return this.loaded;
  }

  renderTabCanvas() {
    return;
  }
}

bottomTabRegistry.register(JankyEventLatenciesDetailsPanel);
