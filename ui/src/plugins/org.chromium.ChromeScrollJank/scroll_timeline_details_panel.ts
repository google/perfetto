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

import m from 'mithril';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {Trace} from '../../public/trace';
import {
  LONG,
  NUM_NULL,
  SqlValue,
  STR,
} from '../../trace_processor/query_result';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Duration, duration, Time, time} from '../../base/time';
import {assertExists, assertTrue} from '../../base/logging';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {Timestamp} from '../../components/widgets/timestamp';
import {DurationWidget} from '../../components/widgets/duration';
import {fromSqlBool, renderSliceRef, renderSqlRef} from './utils';
import SqlModulesPlugin from '../dev.perfetto.SqlModules';
import {
  TableColumn,
  TableManager,
} from '../../components/widgets/sql/table/table_column';
import {renderStandardCell} from '../../components/widgets/sql/table/render_cell_utils';
import {ScrollTimelineModel} from './scroll_timeline_model';
import {
  DurationColumn,
  StandardColumn,
  TimestampColumn,
} from '../../components/widgets/sql/table/columns';

function createPluginSliceIdColumn(
  trace: Trace,
  trackUri: string,
  name: string,
): TableColumn {
  const col = new StandardColumn(name);
  col.renderCell = (value: SqlValue, tableManager: TableManager) => {
    if (value === null || typeof value !== 'bigint') {
      return renderStandardCell(value, name, tableManager);
    }
    return renderSliceRef({
      trace: trace,
      id: Number(value),
      trackUri: trackUri,
      title: `${value}`,
    });
  };
  return col;
}

function createScrollTimelineTableColumns(
  trace: Trace,
  trackUri: string,
): TableColumn[] {
  return [
    createPluginSliceIdColumn(trace, trackUri, 'id'),
    new StandardColumn('scroll_update_id'),
    new TimestampColumn('ts'),
    new DurationColumn('dur'),
    new StandardColumn('name'),
    new StandardColumn('classification'),
  ];
}

export class ScrollTimelineDetailsPanel implements TrackEventDetailsPanel {
  // Information about the scroll update *slice*, which was emitted by
  // ScrollTimelineTrack.
  // Source: this.tableName[id=this.id]
  private sliceData?: {
    name: string;
    ts: time;
    dur: duration;
    // ID of the scroll update in chrome_scroll_update_info.
    scrollUpdateId: bigint;
  };

  // Information about the scroll *update*, which comes from the Chrome tracing
  // stdlib.
  // Source: chrome_scroll_update_info[id=this.sliceData.scrollUpdateId]
  private scrollData?: {
    vsyncInterval: duration | undefined;
    isPresented: boolean | undefined;
    isJanky: boolean | undefined;
    isInertial: boolean | undefined;
    isFirstScrollUpdateInScroll: boolean | undefined;
    isFirstScrollUpdateInFrame: boolean | undefined;
  };

  constructor(
    private readonly trace: Trace,
    private readonly model: ScrollTimelineModel,
    // ID of the slice in tableName.
    private readonly id: number,
  ) {}

  async load(): Promise<void> {
    await this.querySliceData();
    await this.queryScrollData();
  }

  private async querySliceData(): Promise<void> {
    assertTrue(this.sliceData === undefined);
    const queryResult = await this.trace.engine.query(`
      SELECT
        name,
        ts,
        dur,
        scroll_update_id
      FROM ${this.model.tableName}
      WHERE id = ${this.id}`);
    const row = queryResult.firstRow({
      name: STR,
      ts: LONG,
      dur: LONG,
      scroll_update_id: LONG,
    });
    this.sliceData = {
      name: row.name,
      ts: Time.fromRaw(row.ts),
      dur: Duration.fromRaw(row.dur),
      scrollUpdateId: row.scroll_update_id,
    };
  }

  private async queryScrollData(): Promise<void> {
    assertExists(this.sliceData);
    assertTrue(this.scrollData === undefined);
    const queryResult = await this.trace.engine.query(`
      INCLUDE PERFETTO MODULE chrome.chrome_scrolls;
      SELECT
        vsync_interval_ms,
        is_presented,
        is_janky,
        is_inertial,
        is_first_scroll_update_in_scroll,
        is_first_scroll_update_in_frame
      FROM chrome_scroll_update_info
      WHERE id = ${this.sliceData!.scrollUpdateId}`);
    const row = queryResult.firstRow({
      vsync_interval_ms: NUM_NULL,
      is_presented: NUM_NULL,
      is_janky: NUM_NULL,
      is_inertial: NUM_NULL,
      is_first_scroll_update_in_scroll: NUM_NULL,
      is_first_scroll_update_in_frame: NUM_NULL,
    });
    this.scrollData = {
      vsyncInterval:
        row.vsync_interval_ms === null
          ? undefined
          : Duration.fromMillis?.(row.vsync_interval_ms),
      isPresented: fromSqlBool(row.is_presented),
      isJanky: fromSqlBool(row.is_janky),
      isInertial: fromSqlBool(row.is_inertial),
      isFirstScrollUpdateInScroll: fromSqlBool(
        row.is_first_scroll_update_in_scroll,
      ),
      isFirstScrollUpdateInFrame: fromSqlBool(
        row.is_first_scroll_update_in_frame,
      ),
    };
  }

  render(): m.Children {
    return m(
      DetailsShell,
      {
        title: 'Slice',
        description: this.sliceData?.name ?? 'Loading...',
      },
      m(
        GridLayout,
        m(GridLayoutColumn, this.renderSliceDetails()),
        m(GridLayoutColumn, this.renderScrollDetails()),
      ),
    );
  }

  private renderSliceDetails(): m.Child {
    let child;
    if (this.sliceData === undefined) {
      child = 'Loading...';
    } else {
      child = m(
        Tree,
        m(TreeNode, {
          left: 'Name',
          right: this.sliceData.name,
        }),
        m(TreeNode, {
          left: 'Start time',
          right: m(Timestamp, {ts: this.sliceData.ts}),
        }),
        m(TreeNode, {
          left: 'Duration',
          right: m(DurationWidget, {dur: this.sliceData.dur}),
        }),
        m(TreeNode, {
          left: 'SQL ID',
          right: renderSqlRef({
            trace: this.trace,
            tableName: this.model.tableName,
            tableDescription: {
              name: this.model.tableName,
              columns: createScrollTimelineTableColumns(
                this.trace,
                this.model.trackUri,
              ),
            },
            id: this.id,
          }),
        }),
      );
    }
    return m(Section, {title: 'Slice details'}, child);
  }

  private renderScrollDetails(): m.Child {
    let child;
    if (this.sliceData === undefined || this.scrollData === undefined) {
      child = 'Loading...';
    } else {
      const scrollTableDescription = this.trace.plugins
        .getPlugin(SqlModulesPlugin)
        .getSqlModules()
        .getModuleForTable('chrome_scroll_update_info')
        ?.getSqlTableDescription('chrome_scroll_update_info');
      child = m(
        Tree,
        m(TreeNode, {
          left: 'Vsync interval',
          right:
            this.scrollData.vsyncInterval === undefined
              ? `${this.scrollData.vsyncInterval}`
              : m(DurationWidget, {dur: this.scrollData.vsyncInterval}),
        }),
        m(TreeNode, {
          left: 'Is presented',
          right: `${this.scrollData.isPresented}`,
        }),
        m(TreeNode, {
          left: 'Is janky',
          right: `${this.scrollData.isJanky}`,
        }),
        m(TreeNode, {
          left: 'Is inertial',
          right: `${this.scrollData.isInertial}`,
        }),
        m(TreeNode, {
          left: 'Is first scroll update in scroll',
          right: `${this.scrollData.isFirstScrollUpdateInScroll}`,
        }),
        m(TreeNode, {
          left: 'Is first scroll update in frame',
          right: `${this.scrollData.isFirstScrollUpdateInFrame}`,
        }),
        m(TreeNode, {
          left: 'SQL ID',
          right: renderSqlRef({
            trace: this.trace,
            tableName: 'chrome_scroll_update_info',
            id: this.sliceData.scrollUpdateId,
            tableDescription: scrollTableDescription,
          }),
        }),
      );
    }
    return m(Section, {title: 'Scroll details'}, child);
  }
}
