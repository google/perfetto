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
import {
  BottomTab,
  bottomTabRegistry,
  NewBottomTabArgs,
} from '../../frontend/bottom_tab';
import {
  GenericSliceDetailsTabConfig,
} from '../../frontend/generic_slice_details_tab';
import {DurationWidget} from '../../frontend/widgets/duration';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {LONG, NUM, STR, STR_NULL} from '../../trace_processor/query_result';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {SqlRef} from '../../widgets/sql_ref';
import {dictToTreeNodes, Tree} from '../../widgets/tree';
import {asUpid, Upid} from '../../frontend/sql_types';

interface Data {
  startupId: number;
  eventName: string;
  startupBeginTs: time;
  durToFirstVisibleContent: duration;
  launchCause?: string;
  upid: Upid;
}

export class StartupDetailsPanel extends
  BottomTab<GenericSliceDetailsTabConfig> {
  static readonly kind = 'org.perfetto.StartupDetailsPanel';
  private loaded = false;
  private data: Data|undefined;

  static create(args: NewBottomTabArgs<GenericSliceDetailsTabConfig>):
      StartupDetailsPanel {
    return new StartupDetailsPanel(args);
  }

  constructor(args: NewBottomTabArgs<GenericSliceDetailsTabConfig>) {
    super(args);
    this.loadData();
  }

  private async loadData() {
    const queryResult = await this.engine.query(`
      SELECT
        activity_id AS startupId,
        name,
        startup_begin_ts AS startupBeginTs,
        CASE
          WHEN first_visible_content_ts IS NULL THEN 0
          ELSE first_visible_content_ts - startup_begin_ts
        END AS durTofirstVisibleContent,
        launch_cause AS launchCause,
        browser_upid AS upid
      FROM chrome_startups
      WHERE id = ${this.config.id};
    `);

    const iter = queryResult.firstRow({
      startupId: NUM,
      name: STR,
      startupBeginTs: LONG,
      durTofirstVisibleContent: LONG,
      launchCause: STR_NULL,
      upid: NUM,
    });

    this.data = {
      startupId: iter.startupId,
      eventName: iter.name,
      startupBeginTs: Time.fromRaw(iter.startupBeginTs),
      durToFirstVisibleContent: iter.durTofirstVisibleContent,
      upid: asUpid(iter.upid),
    };

    if (iter.launchCause) {
      this.data.launchCause = iter.launchCause;
    }

    this.loaded = true;
  }

  private getDetailsDictionary() {
    const details: {[key: string]: m.Child} = {};
    if (this.data === undefined) return details;
    details['Activity ID'] = this.data.startupId;
    details['Browser Upid'] = this.data.upid;
    details['Startup Event'] = this.data.eventName;
    details['Startup Timestamp'] = m(Timestamp, {ts: this.data.startupBeginTs});
    details['Duration to First Visible Content'] =
        m(DurationWidget, {dur: this.data.durToFirstVisibleContent});
    if (this.data.launchCause) {
      details['Launch Cause'] = this.data.launchCause;
    }
    details['SQL ID'] =
        m(SqlRef, {table: 'chrome_startups', id: this.config.id});
    return details;
  }

  viewTab() {
    if (this.isLoading()) {
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
            m(Tree, dictToTreeNodes(this.getDetailsDictionary())),
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

bottomTabRegistry.register(StartupDetailsPanel);
