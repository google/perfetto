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
import {
  CustomSqlTableDefConfig,
  CustomSqlTableSliceTrack,
} from '../../../frontend/tracks/custom_sql_table_slice_track';
import {TrackContext} from '../../track';
import {Button} from '../../../widgets/button';
import {Icons} from '../../../base/semantic_icons';
import {Trace} from '../../trace';
import {TrackEventSelection} from '../../selection';
import {DebugSliceDetailsPanel} from './details_tab';

export class DebugSliceTrack extends CustomSqlTableSliceTrack {
  private readonly sqlTableName: string;

  constructor(trace: Trace, ctx: TrackContext, tableName: string) {
    super({
      trace,
      uri: ctx.trackUri,
    });
    this.sqlTableName = tableName;
  }

  async getSqlDataSource(): Promise<CustomSqlTableDefConfig> {
    return {
      sqlTableName: this.sqlTableName,
    };
  }

  getTrackShellButtons(): m.Children {
    return m(Button, {
      onclick: () => {
        this.trace.workspace.findTrackByUri(this.uri)?.remove();
      },
      icon: Icons.Close,
      title: 'Close',
      compact: true,
    });
  }

  detailsPanel(sel: TrackEventSelection) {
    return new DebugSliceDetailsPanel(this.trace, this.tableName, sel.eventId);
  }
}
