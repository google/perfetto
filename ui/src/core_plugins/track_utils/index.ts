// Copyright (C) 2024 The Android Open Source Project
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

import z from 'zod';
import {OmniboxMode} from '../../core/omnibox_manager';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {AppImpl} from '../../core/app_impl';
import {getTimeSpanOfSelectionOrVisibleWindow} from '../../public/utils';
import {exists, RequiredField} from '../../base/utils';
import {LONG, NUM, NUM_NULL} from '../../trace_processor/query_result';
import {TrackNode} from '../../public/workspace';
import {App} from '../../public/app';
import {Setting} from '../../public/settings';

export default class TrackUtilsPlugin implements PerfettoPlugin {
  static readonly id = 'perfetto.TrackUtils';
  static dvorakSetting: Setting<boolean>;

  static onActivate(ctx: App): void {
    TrackUtilsPlugin.dvorakSetting = ctx.settings.register({
      // Plugin ID is omitted because we might want to move this setting in the
      // future.
      id: 'dvorakMode',
      defaultValue: false,
      name: 'Dvorak mode',
      description: 'Rearranges hotkeys to avoid collisions in Dvorak layout.',
      schema: z.boolean(),
      requiresReload: true, // Hotkeys are registered on trace load.
    });
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    ctx.commands.registerCommand({
      id: 'perfetto.RunQueryInSelectedTimeWindow',
      name: `Run query in selected time window`,
      callback: async () => {
        const window = await getTimeSpanOfSelectionOrVisibleWindow(ctx);
        const omnibox = AppImpl.instance.omnibox;
        omnibox.setMode(OmniboxMode.Query);
        omnibox.setText(
          `select  where ts >= ${window.start} and ts < ${window.end}`,
        );
        omnibox.focus(/* cursorPlacement= */ 7);
      },
    });

    ctx.commands.registerCommand({
      id: 'perfetto.FindTrackByName',
      name: 'Find track by name',
      callback: async () => {
        const tracksWithUris = ctx.workspace.flatTracksOrdered.filter(
          (track) => track.uri !== undefined,
        ) as ReadonlyArray<RequiredField<TrackNode, 'uri'>>;
        const track = await ctx.omnibox.prompt('Choose a track...', {
          values: tracksWithUris,
          getName: (track) => track.title,
        });
        track &&
          ctx.selection.selectTrack(track.uri, {
            scrollToSelection: true,
          });
      },
    });

    ctx.commands.registerCommand({
      id: 'perfetto.FindTrackByUri',
      name: 'Find track by URI',
      callback: async () => {
        const tracksWithUris = ctx.workspace.flatTracksOrdered.filter(
          (track) => track.uri !== undefined,
        ) as ReadonlyArray<RequiredField<TrackNode, 'uri'>>;
        const track = await ctx.omnibox.prompt('Choose a track...', {
          values: tracksWithUris,
          getName: (track) => track.uri,
        });
        track &&
          ctx.selection.selectTrack(track.uri, {
            scrollToSelection: true,
          });
      },
    });

    ctx.commands.registerCommand({
      id: 'perfetto.PinTrackByName',
      name: 'Pin track by name',
      defaultHotkey: 'Shift+T',
      callback: async () => {
        const tracksWithUris = ctx.workspace.flatTracksOrdered.filter(
          (track) => track.uri !== undefined,
        ) as ReadonlyArray<RequiredField<TrackNode, 'uri'>>;
        const track = await ctx.omnibox.prompt('Choose a track...', {
          values: tracksWithUris,
          getName: (track) => track.title,
        });
        track && track.pin();
      },
    });

    ctx.commands.registerCommand({
      id: 'perfetto.SelectNextTrackEvent',
      name: 'Select next track event',
      defaultHotkey: '.',
      callback: async () => {
        await selectAdjacentTrackEvent(ctx, 'next');
      },
    });

    ctx.commands.registerCommand({
      id: 'perfetto.SelectPreviousTrackEvent',
      name: 'Select previous track event',
      defaultHotkey: !TrackUtilsPlugin.dvorakSetting.get() ? ',' : undefined,
      callback: async () => {
        await selectAdjacentTrackEvent(ctx, 'prev');
      },
    });
  }
}

/**
 * If a track event is currently selected, select the next or previous event on
 * that same track chronologically ordered by `ts`.
 */
async function selectAdjacentTrackEvent(
  ctx: Trace,
  direction: 'next' | 'prev',
) {
  const selection = ctx.selection.selection;
  if (selection.kind !== 'track_event') return;

  const td = ctx.tracks.getTrack(selection.trackUri);
  const dataset = td?.track.getDataset?.();
  if (!dataset || !dataset.implements({id: NUM, ts: LONG})) return;

  const windowFunc = direction === 'next' ? 'LEAD' : 'LAG';
  const result = await ctx.engine.query(`
      WITH
        CTE AS (
          SELECT
            id,
            ${windowFunc}(id) OVER (ORDER BY ts) AS resultId
          FROM (${dataset.query()})
        )
      SELECT * FROM CTE WHERE id = ${selection.eventId}
    `);
  const resultId = result.maybeFirstRow({resultId: NUM_NULL})?.resultId;
  if (!exists(resultId)) return;

  ctx.selection.selectTrackEvent(selection.trackUri, resultId, {
    scrollToSelection: true,
  });
}
