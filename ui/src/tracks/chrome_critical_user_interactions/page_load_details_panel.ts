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

import {duration, time, Time} from '../../base/time';
import {exists} from '../../base/utils';
import {raf} from '../../core/raf_scheduler';
import {
  BottomTab,
  bottomTabRegistry,
  NewBottomTabArgs,
} from '../../frontend/bottom_tab';
import {
  GenericSliceDetailsTabConfig,
} from '../../frontend/generic_slice_details_tab';
import {asUpid, Upid} from '../../frontend/sql_types';
import {DurationWidget} from '../../frontend/widgets/duration';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {LONG, LONG_NULL, NUM, STR} from '../../trace_processor/query_result';
import {Anchor} from '../../widgets/anchor';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {SqlRef} from '../../widgets/sql_ref';
import {dictToTreeNodes, Tree} from '../../widgets/tree';

interface Data {
  ts: time;
  url: string;
  // The row id in the chrome_page_loads table is the unique identifier of the
  // combination of navigation id and browser upid; otherwise, navigation id
  // is not guaranteed to be unique in a trace.
  id: number;
  navigationId: number;
  upid: Upid;
  fcpDuration: duration;
  lcpDuration?: duration;
  fcpTs: time;
  lcpTs?: time;
  domContentLoadedTs?: time;
  loadTs?: time;
  markFullyLoadedTs?: time;
  markFullyVisibleTs?: time;
  markInteractiveTs?: time;
}

export class PageLoadDetailsPanel extends
    BottomTab<GenericSliceDetailsTabConfig> {
  static readonly kind = 'org.perfetto.PageLoadDetailsPanel';
  private loaded = false;
  private data: Data|undefined;

  static create(args: NewBottomTabArgs): PageLoadDetailsPanel {
    return new PageLoadDetailsPanel(args);
  }

  constructor(args: NewBottomTabArgs) {
    super(args);
    this.loadData();
  }

  private async loadData() {
    const queryResult = await this.engine.query(`
      SELECT
        id,
        navigation_id AS navigationId,
        browser_upid AS upid,
        navigation_start_ts AS ts,
        url,
        fcp AS fcpDuration,
        lcp AS lcpDuration,
        fcp_ts AS fcpTs,
        lcp_ts AS lcpTs,
        dom_content_loaded_event_ts AS domContentLoadedTs,
        load_event_ts AS loadTs,
        mark_fully_loaded_ts AS markFullyLoadedTs,
        mark_fully_visible_ts AS markFullyVisibleTs,
        mark_interactive_ts AS markInteractiveTs
      FROM chrome_page_loads
      WHERE id = ${this.config.id};`);

    const iter = queryResult.firstRow({
      id: NUM,
      navigationId: NUM,
      upid: NUM,
      ts: LONG,
      url: STR,
      fcpDuration: LONG,
      lcpDuration: LONG_NULL,
      fcpTs: LONG,
      lcpTs: LONG_NULL,
      domContentLoadedTs: LONG_NULL,
      loadTs: LONG_NULL,
      markFullyLoadedTs: LONG_NULL,
      markFullyVisibleTs: LONG_NULL,
      markInteractiveTs: LONG_NULL,
    });

    this.data = {
      id: iter.id,
      ts: Time.fromRaw(iter.ts),
      fcpTs: Time.fromRaw(iter.fcpTs),
      fcpDuration: iter.fcpDuration,
      navigationId: iter.navigationId,
      upid: asUpid(iter.upid),
      url: iter.url,
      lcpTs: Time.fromRaw(iter.lcpTs ?? undefined),
      lcpDuration: iter.lcpDuration ?? undefined,
      domContentLoadedTs: Time.fromRaw(iter.domContentLoadedTs ?? undefined),
      loadTs: Time.fromRaw(iter.loadTs ?? undefined),
      markFullyLoadedTs: Time.fromRaw(iter.markFullyLoadedTs ?? undefined),
      markFullyVisibleTs: Time.fromRaw(iter.markFullyVisibleTs ?? undefined),
      markInteractiveTs: Time.fromRaw(iter.markInteractiveTs ?? undefined),
    };

    this.loaded = true;
    raf.scheduleFullRedraw();
  }

  private getDetailsDictionary() {
    const details: {[key: string]: m.Child} = {};
    if (exists(this.data)) {
      details['Timestamp'] = m(Timestamp, {ts: this.data.ts});
      details['FCP Timestamp'] = m(Timestamp, {ts: this.data.fcpTs});
      details['FCP Duration'] = m(DurationWidget, {dur: this.data.fcpDuration});

      if (exists(this.data.lcpTs)) {
        details['LCP Timestamp'] = m(Timestamp, {ts: this.data.lcpTs});
      }
      if (exists(this.data.lcpDuration)) {
        details['LCP Duration'] =
            m(DurationWidget, {dur: this.data.lcpDuration});
      }

      if (exists(this.data.domContentLoadedTs)) {
        details['DOM Content Loaded Event Timestamp'] =
            m(Timestamp, {ts: this.data.domContentLoadedTs});
      }

      if (exists(this.data.loadTs)) {
        details['Load Timestamp'] = m(Timestamp, {ts: this.data.loadTs});
      }

      if (exists(this.data.markFullyLoadedTs)) {
        details['Page Timing Mark Fully Loaded Timestamp'] =
            m(Timestamp, {ts: this.data.markFullyLoadedTs});
      }

      if (exists(this.data.markFullyVisibleTs)) {
        details['Page Timing Mark Fully Visible Timestamp'] =
            m(Timestamp, {ts: this.data.markFullyVisibleTs});
      }

      if (exists(this.data.markInteractiveTs)) {
        details['Page Timing Mark Interactive Timestamp'] =
            m(Timestamp, {ts: this.data.markInteractiveTs});
      }

      details['Navigation ID'] = this.data.navigationId;
      details['Browser Upid'] = this.data.upid;
      details['URL'] =
          m(Anchor,
            {href: this.data.url, target: '_blank', icon: 'open_in_new'},
            this.data.url);
      details['SQL ID'] =
          m(SqlRef, {table: 'chrome_page_loads', id: this.data.id});
    }
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

bottomTabRegistry.register(PageLoadDetailsPanel);
