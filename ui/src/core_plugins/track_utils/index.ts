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

import {OmniboxMode} from '../../core/omnibox_manager';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {AppImpl} from '../../core/app_impl';
import {getTimeSpanOfSelectionOrVisibleWindow} from '../../public/utils';
import {exists} from '../../base/utils';
import {LONG, NUM, NUM_NULL} from '../../trace_processor/query_result';

export default class implements PerfettoPlugin {
  static readonly id = 'perfetto.TrackUtils';
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
        const options = ctx.workspace.flatTracks
          .map((node) => {
            return exists(node.uri)
              ? {key: node.uri, displayName: node.fullPath.join(' \u2023 ')}
              : undefined;
          })
          .filter((pair) => pair !== undefined);
        const uri = await ctx.omnibox.prompt('Choose a track...', options);
        uri && ctx.selection.selectTrack(uri, {scrollToSelection: true});
      },
    });

    ctx.commands.registerCommand({
      id: 'perfetto.FindTrackByUri',
      name: 'Find track by URI',
      callback: async () => {
        const options = ctx.workspace.flatTracks
          .map((track) => track.uri)
          .filter((uri) => uri !== undefined)
          .map((uri) => {
            return {key: uri, displayName: uri};
          });

        const uri = await ctx.omnibox.prompt('Choose a track...', options);
        uri && ctx.selection.selectTrack(uri, {scrollToSelection: true});
      },
    });

    ctx.commands.registerCommand({
      id: 'perfetto.PinTrackByName',
      name: 'Pin track by name',
      callback: async () => {
        const options = ctx.workspace.flatTracks
          .map((node) => {
            return exists(node.uri)
              ? {key: node.id, displayName: node.fullPath.join(' \u2023 ')}
              : undefined;
          })
          .filter((option) => option !== undefined);
        const id = await ctx.omnibox.prompt('Choose a track...', options);
        id && ctx.workspace.getTrackById(id)?.pin();
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
      defaultHotkey: ',',
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
