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

import {Actions} from '../../common/actions';
import {
  generateSqlWithInternalLayout,
} from '../../common/internal_layout_utils';
import {Selection} from '../../common/state';
import {OnSliceClickArgs} from '../../frontend/base_slice_track';
import {
  GenericSliceDetailsTabConfigBase,
} from '../../frontend/generic_slice_details_tab';
import {globals} from '../../frontend/globals';
import {
  NamedSliceTrack,
  NamedSliceTrackTypes,
} from '../../frontend/named_slice_track';
import {NewTrackArgs} from '../../frontend/track';
import {Plugin, PluginContext, PluginDescriptor} from '../../public';

export interface CustomSqlTableDefConfig {
  // Table name
  sqlTableName: string;
  // Table columns
  columns?: string[];
  whereClause?: string;
}

export interface CustomSqlDetailsPanelConfig {
  // Type of details panel to create
  kind: string;
  // Config for the details panel
  config: GenericSliceDetailsTabConfigBase;
}

export abstract class CustomSqlTableSliceTrack<
    T extends NamedSliceTrackTypes> extends NamedSliceTrack<T> {
  constructor(args: NewTrackArgs) {
    super(args);
  }

  abstract getSqlDataSource(): CustomSqlTableDefConfig;

  // Override by subclasses.
  abstract getDetailsPanel(): CustomSqlDetailsPanelConfig;

  async initSqlTable(tableName: string) {
    const config = this.getSqlDataSource();
    let columns = ['*'];
    if (config.columns !== undefined) {
      columns = config.columns;
    }

    const sql = `CREATE VIEW ${tableName} AS ` + generateSqlWithInternalLayout({
                  columns: columns,
                  sourceTable: config.sqlTableName,
                  ts: 'ts',
                  dur: 'dur',
                  whereClause: config.whereClause,
                });

    await this.engine.query(sql);
  }

  isSelectionHandled(selection: Selection) {
    if (selection.kind !== 'GENERIC_SLICE') {
      return false;
    }
    return selection.trackId === this.trackId;
  }

  onSliceClick(args: OnSliceClickArgs<NamedSliceTrackTypes['slice']>) {
    if (this.getDetailsPanel() === undefined) {
      return;
    }

    const detailsPanelConfig = this.getDetailsPanel();
    globals.makeSelection(Actions.selectGenericSlice({
      id: args.slice.id,
      sqlTableName: this.tableName,
      start: args.slice.ts,
      duration: args.slice.dur,
      trackId: this.trackId,
      detailsPanelConfig: {
        kind: detailsPanelConfig.kind,
        config: detailsPanelConfig.config,
      },
    }));
  }
}

class CustomSqlTrackPlugin implements Plugin {
  onActivate(ctx: PluginContext): void {
    // noop to allow directory to compile.
    if (ctx) {
      return;
    }
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.CustomSqlTrack',
  plugin: CustomSqlTrackPlugin,
};
