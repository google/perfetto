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

import {duration, Time, time} from '../../base/time';
import {exists} from '../../base/utils';
import {LONG, LONG_NULL, NUM, STR} from '../../common/query_result';
import {raf} from '../../core/raf_scheduler';
import {
  BottomTab,
  bottomTabRegistry,
  NewBottomTabArgs,
} from '../../frontend/bottom_tab';
import {
  GenericSliceDetailsTabConfig,
} from '../../frontend/generic_slice_details_tab';
import {sqlValueToString} from '../../frontend/sql_utils';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {Anchor} from '../../widgets/anchor';
import {DetailsShell} from '../../widgets/details_shell';
import {DurationWidget} from '../../widgets/duration';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {SqlRef} from '../../widgets/sql_ref';
import {dictToTreeNodes, Tree} from '../../widgets/tree';

interface PageLoadMetrics {
  url: string;
  navigationId: number;
  fcpDuration?: duration;
  lcpDuration?: duration;
  fcpTs: time, lcpTs?: time,
}

enum CriticalUserJourneyType {
  UNKNOWN = 'Unknown',
  PAGE_LOAD = 'PageLoad',
}

function convertToCriticalUserJourneyType(cujType: string):
    CriticalUserJourneyType {
  switch (cujType) {
    case CriticalUserJourneyType.PAGE_LOAD:
      return CriticalUserJourneyType.PAGE_LOAD;
    default:
      return CriticalUserJourneyType.UNKNOWN;
  }
}

interface Data {
  name: string;
  // Timestamp of the beginning of this slice in nanoseconds.
  ts: time;
  // Duration of this slice in nanoseconds.
  dur: duration;
  type: CriticalUserJourneyType;
  tableName: string;
  // Metrics for |type| = CriticalUserJourney.PAGE_LOAD
  pageLoadMetrics?: PageLoadMetrics;
}

export class CriticalUserInteractionDetailsPanel extends
    BottomTab<GenericSliceDetailsTabConfig> {
  static readonly kind = 'org.perfetto.CriticalUserInteractionDetailsPanel';
  data: Data|undefined;
  loaded = false;

  static create(args: NewBottomTabArgs): CriticalUserInteractionDetailsPanel {
    return new CriticalUserInteractionDetailsPanel(args);
  }

  constructor(args: NewBottomTabArgs) {
    super(args);
    this.loadData();
  }

  private async loadData() {
    const queryResult = await this.engine.query(`
      SELECT
        name,
        ts,
        dur,
        type AS tableName
      FROM chrome_interactions
      WHERE scoped_id = ${this.config.id}`);

    const iter = queryResult.firstRow({
      name: STR,
      ts: LONG,
      dur: LONG,
      tableName: STR,
    });

    this.data = {
      name: iter.name,
      ts: Time.fromRaw(iter.ts),
      dur: iter.dur,
      type: convertToCriticalUserJourneyType(iter.name),
      tableName: iter.tableName,
    };

    await this.loadMetrics();

    this.loaded = true;
    raf.scheduleFullRedraw();
  }

  private async loadMetrics() {
    if (exists(this.data)) {
      switch (this.data.type) {
        case CriticalUserJourneyType.PAGE_LOAD:
          await this.loadPageLoadMetrics();
          break;
        default:
          break;
      }
    }
  }

  private async loadPageLoadMetrics() {
    if (exists(this.data)) {
      const queryResult = await this.engine.query(`
      SELECT
        navigation_id AS navigationId,
        url,
        fcp AS fcpDuration,
        lcp AS lcpDuration,
        fcp_ts AS fcpTs,
        lcp_ts AS lcpTs
      FROM chrome_page_loads
      WHERE navigation_id = ${this.config.id}`);

      const iter = queryResult.firstRow({
        navigationId: NUM,
        url: STR,
        fcpDuration: LONG_NULL,
        lcpDuration: LONG_NULL,
        fcpTs: LONG,
        lcpTs: LONG,
      });

      this.data.pageLoadMetrics = {
        navigationId: iter.navigationId,
        url: iter.url,
        fcpTs: Time.fromRaw(iter.fcpTs),
      };

      if (exists(iter.fcpDuration)) {
        this.data.pageLoadMetrics.fcpDuration = iter.fcpDuration;
      }

      if (exists(iter.lcpDuration)) {
        this.data.pageLoadMetrics.lcpDuration = iter.lcpDuration;
      }

      if (Number(iter.lcpTs) != 0) {
        this.data.pageLoadMetrics.lcpTs = Time.fromRaw(iter.lcpTs);
      }
    }
  }

  private renderDetailsDictionary(): m.Child[] {
    const details: {[key: string]: m.Child} = {};
    if (exists(this.data)) {
      details['Name'] = sqlValueToString(this.data.name);
      details['Timestamp'] = m(Timestamp, {ts: this.data.ts});
      if (exists(this.data.pageLoadMetrics)) {
        details['FCP Timestamp'] =
            m(Timestamp, {ts: this.data.pageLoadMetrics.fcpTs});
        if (exists(this.data.pageLoadMetrics.fcpDuration)) {
          details['FCP Duration'] =
              m(DurationWidget, {dur: this.data.pageLoadMetrics.fcpDuration});
        }
        if (exists(this.data.pageLoadMetrics.lcpTs)) {
          details['LCP Timestamp'] =
              m(Timestamp, {ts: this.data.pageLoadMetrics.lcpTs});
        }
        if (exists(this.data.pageLoadMetrics.lcpDuration)) {
          details['LCP Duration'] =
              m(DurationWidget, {dur: this.data.pageLoadMetrics.lcpDuration});
        }
        details['Navigation ID'] = this.data.pageLoadMetrics.navigationId;
        const url = this.data.pageLoadMetrics.url;
        details['URL'] =
            m(Anchor, {href: url, target: '_blank', icon: 'open_in_new'}, url);
      }
      details['SQL ID'] =
          m(SqlRef, {table: 'chrome_interactions', id: this.config.id});
    }

    return dictToTreeNodes(details);
  }

  viewTab() {
    if (this.data === undefined) {
      return m('h2', 'Loading');
    }

    return m(
        DetailsShell,
        {
          title: this.getTitle(),
        },
        m(GridLayout,
          m(
              GridLayoutColumn,
              m(
                  Section,
                  {title: 'Details'},
                  m(Tree, this.renderDetailsDictionary()),
                  ),
              )));
  }

  getTitle(): string {
    return this.config.title;
  }

  isLoading() {
    return !this.loaded;
  }
}

bottomTabRegistry.register(CriticalUserInteractionDetailsPanel);
