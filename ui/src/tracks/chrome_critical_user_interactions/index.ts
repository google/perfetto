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

import {v4 as uuidv4} from 'uuid';

import {Actions} from '../../common/actions';
import {SCROLLING_TRACK_GROUP} from '../../common/state';
import {OnSliceClickArgs} from '../../frontend/base_slice_track';
import {
  GenericSliceDetailsTab,
  GenericSliceDetailsTabConfig,
} from '../../frontend/generic_slice_details_tab';
import {globals} from '../../frontend/globals';
import {
  NAMED_ROW,
  NamedSliceTrackTypes,
} from '../../frontend/named_slice_track';
import {
  BottomTabToSCSAdapter,
  NUM,
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
  PrimaryTrackSortKey,
  Slice,
  STR,
} from '../../public';
import {
  CustomSqlDetailsPanelConfig,
  CustomSqlImportConfig,
  CustomSqlTableDefConfig,
  CustomSqlTableSliceTrack,
} from '../custom_sql_table_slices';

import {PageLoadDetailsPanel} from './page_load_details_panel';
import {StartupDetailsPanel} from './startup_details_panel';
import {
  WebContentInteractionPanel,
} from './web_content_interaction_details_panel';

export const CRITICAL_USER_INTERACTIONS_KIND =
    'org.chromium.CriticalUserInteraction.track';

export const CRITICAL_USER_INTERACTIONS_ROW = {
  ...NAMED_ROW,
  scopedId: NUM,
  type: STR,
};
export type CriticalUserInteractionRow = typeof CRITICAL_USER_INTERACTIONS_ROW;

export interface CriticalUserInteractionSlice extends Slice {
  scopedId: number;
  type: string;
}

export interface CriticalUserInteractionSliceTrackTypes extends
    NamedSliceTrackTypes {
  slice: CriticalUserInteractionSlice;
  row: CriticalUserInteractionRow;
}

enum CriticalUserInteractionType {
  UNKNOWN = 'Unknown',
  PAGE_LOAD = 'chrome_page_loads',
  STARTUP = 'chrome_startups',
  WEB_CONTENT_INTERACTION = 'chrome_web_content_interactions',
}

function convertToCriticalUserInteractionType(cujType: string):
    CriticalUserInteractionType {
  switch (cujType) {
  case CriticalUserInteractionType.PAGE_LOAD:
    return CriticalUserInteractionType.PAGE_LOAD;
  case CriticalUserInteractionType.STARTUP:
    return CriticalUserInteractionType.STARTUP;
  case CriticalUserInteractionType.WEB_CONTENT_INTERACTION:
    return CriticalUserInteractionType.WEB_CONTENT_INTERACTION;
  default:
    return CriticalUserInteractionType.UNKNOWN;
  }
}

export class CriticalUserInteractionTrack extends
  CustomSqlTableSliceTrack<CriticalUserInteractionSliceTrackTypes> {
  static readonly kind = CRITICAL_USER_INTERACTIONS_KIND;

  getSqlDataSource(): CustomSqlTableDefConfig {
    return {
      columns: [
        // The scoped_id is not a unique identifier within the table; generate
        // a unique id from type and scoped_id on the fly to use for slice
        // selection.
        'hash(type, scoped_id) AS id',
        'scoped_id AS scopedId',
        'name',
        'ts',
        'dur',
        'type',
      ],
      sqlTableName: 'chrome_interactions',
    };
  }

  getDetailsPanel(
    args: OnSliceClickArgs<CriticalUserInteractionSliceTrackTypes['slice']>):
      CustomSqlDetailsPanelConfig {
    let detailsPanel = {
      kind: GenericSliceDetailsTab.kind,
      config: {
        sqlTableName: this.tableName,
        title: 'Chrome Interaction',
      },
    };

    switch (convertToCriticalUserInteractionType(args.slice.type)) {
    case CriticalUserInteractionType.PAGE_LOAD:
      detailsPanel = {
        kind: PageLoadDetailsPanel.kind,
        config: {
          sqlTableName: this.tableName,
          title: 'Chrome Page Load',
        },
      };
      break;
    case CriticalUserInteractionType.STARTUP:
      detailsPanel = {
        kind: StartupDetailsPanel.kind,
        config: {
          sqlTableName: this.tableName,
          title: 'Chrome Startup',
        },
      };
      break;
    case CriticalUserInteractionType.WEB_CONTENT_INTERACTION:
      detailsPanel = {
        kind: WebContentInteractionPanel.kind,
        config: {
          sqlTableName: this.tableName,
          title: 'Chrome Web Content Interaction',
        },
      };
      break;
    default:
      break;
    }
    return detailsPanel;
  }

  onSliceClick(
    args: OnSliceClickArgs<CriticalUserInteractionSliceTrackTypes['slice']>) {
    const detailsPanelConfig = this.getDetailsPanel(args);
    globals.makeSelection(Actions.selectGenericSlice({
      id: args.slice.scopedId,
      sqlTableName: this.tableName,
      start: args.slice.ts,
      duration: args.slice.dur,
      trackKey: this.trackKey,
      detailsPanelConfig: {
        kind: detailsPanelConfig.kind,
        config: detailsPanelConfig.config,
      },
    }));
  }

  getSqlImports(): CustomSqlImportConfig {
    return {
      modules: ['chrome.interactions'],
    };
  }

  getRowSpec(): CriticalUserInteractionSliceTrackTypes['row'] {
    return CRITICAL_USER_INTERACTIONS_ROW;
  }

  rowToSlice(row: CriticalUserInteractionSliceTrackTypes['row']):
      CriticalUserInteractionSliceTrackTypes['slice'] {
    const baseSlice = super.rowToSlice(row);
    const scopedId = row.scopedId;
    const type = row.type;
    return {...baseSlice, scopedId, type};
  }
}

export function addCriticalUserInteractionTrack() {
  const trackKey = uuidv4();
  globals.dispatchMultiple([
    Actions.addTrack({
      key: trackKey,
      uri: CriticalUserInteractionTrack.kind,
      name: `Chrome Interactions`,
      trackSortKey: PrimaryTrackSortKey.DEBUG_TRACK,
      trackGroup: SCROLLING_TRACK_GROUP,
    }),
    Actions.toggleTrackPinned({trackKey}),
  ]);
}

class CriticalUserInteractionPlugin implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    ctx.registerTrack({
      uri: CriticalUserInteractionTrack.kind,
      kind: CriticalUserInteractionTrack.kind,
      displayName: 'Chrome Interactions',
      trackFactory: (trackCtx) => new CriticalUserInteractionTrack(
        {engine: ctx.engine, trackKey: trackCtx.trackKey}),
    });

    ctx.registerDetailsPanel(new BottomTabToSCSAdapter({
      tabFactory: (selection) => {
        if (selection.kind === 'GENERIC_SLICE' &&
            selection.detailsPanelConfig.kind === PageLoadDetailsPanel.kind) {
          const config = selection.detailsPanelConfig.config;
          return new PageLoadDetailsPanel({
            config: config as GenericSliceDetailsTabConfig,
            engine: ctx.engine,
            uuid: uuidv4(),
          });
        }
        return undefined;
      },
    }));

    ctx.registerDetailsPanel(new BottomTabToSCSAdapter({
      tabFactory: (selection) => {
        if (selection.kind === 'GENERIC_SLICE' &&
            selection.detailsPanelConfig.kind === StartupDetailsPanel.kind) {
          const config = selection.detailsPanelConfig.config;
          return new StartupDetailsPanel({
            config: config as GenericSliceDetailsTabConfig,
            engine: ctx.engine,
            uuid: uuidv4(),
          });
        }
        return undefined;
      },
    }));

    ctx.registerDetailsPanel(new BottomTabToSCSAdapter({
      tabFactory: (selection) => {
        if (selection.kind === 'GENERIC_SLICE' &&
            selection.detailsPanelConfig.kind ===
                WebContentInteractionPanel.kind) {
          const config = selection.detailsPanelConfig.config;
          return new WebContentInteractionPanel({
            config: config as GenericSliceDetailsTabConfig,
            engine: ctx.engine,
            uuid: uuidv4(),
          });
        }
        return undefined;
      },
    }));
  }

  onActivate(ctx: PluginContext): void {
    ctx.registerCommand({
      id: 'perfetto.CriticalUserInteraction.AddInteractionTrack',
      name: 'Add Chrome Interactions track',
      callback: () => addCriticalUserInteractionTrack(),
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.CriticalUserInteraction',
  plugin: CriticalUserInteractionPlugin,
};
