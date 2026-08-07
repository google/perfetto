// Copyright (C) 2026 The Android Open Source Project
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
import type {TraceImpl} from '../../core/trace_impl';
import type {PerfettoPlugin} from '../../public/plugin';
import {FlowEventsAreaSelectedPanel} from './flow_events_panel';
import {renderFlows} from './flow_events_renderer';
import {FlowManager} from './flow_manager';

// Any track that wants to use this plugin to render flows should
export const FLOWS_SLICE_TRACK = 'flows_slice_track';

export default class FlowEventsPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.FlowEvents';
  static readonly description =
    'Handles rendering flows on top of slice tracks originating from the ' +
    'slice table, and for the flows area selection panel.';

  async onTraceLoad(trace: TraceImpl): Promise<void> {
    const flows = new FlowManager(
      trace.engine,
      trace.tracks,
      trace.selection,
      trace.raf,
    );

    trace.tracks.registerOverlay({
      render(ctx, timescale, size, tracks) {
        flows.updateFlows(trace.selection.selection);
        renderFlows(
          trace,
          flows,
          ctx,
          size,
          tracks,
          trace.workspaces.currentWorkspace.tracks,
          timescale,
        );
      },
    });

    // This type assertion is allowed because we're a core plugin.
    trace.selection.registerAreaSelectionTab({
      id: 'flow_events',
      name: 'Flow Events',
      priority: -100,
      render() {
        if (flows.selectedFlows.length > 0) {
          return {
            isLoading: false,
            content: m(FlowEventsAreaSelectedPanel, {trace, flows}),
          };
        } else {
          return undefined;
        }
      },
    });

    trace.commands.registerCommand({
      id: 'dev.perfetto.NextFlow',
      name: 'Next flow',
      callback: () => flows.focusOtherFlow('Forward'),
      defaultHotkey: 'Mod+]',
    });

    trace.commands.registerCommand({
      id: 'dev.perfetto.PrevFlow',
      name: 'Prev flow',
      callback: () => flows.focusOtherFlow('Backward'),
      defaultHotkey: 'Mod+[',
    });

    trace.commands.registerCommand({
      id: 'dev.perfetto.MoveNextFlow',
      name: 'Move next flow',
      callback: () => flows.moveByFocusedFlow('Forward'),
      defaultHotkey: ']',
    });

    trace.commands.registerCommand({
      id: 'dev.perfetto.MovePrevFlow',
      name: 'Move prev flow',
      callback: () => flows.moveByFocusedFlow('Backward'),
      defaultHotkey: '[',
    });
  }
}
