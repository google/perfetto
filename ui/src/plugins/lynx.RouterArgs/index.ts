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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {PerfettoPlugin} from '../../public/plugin';
import {App} from '../../public/app';
import {Trace} from '../../public/trace';
import {NUM} from '../../trace_processor/query_result';

/**
 * Plugin to handle router arguments and control UI components accordingly.
 */
export default class LynxRouterArgsPlugin implements PerfettoPlugin {
  static readonly id = 'lynx.RouterArgs';
  private static selectSliceId = -1;
  private static selectEventName = '';
  static async onActivate(ctx: App) {
    const args = ctx.initialRouteArgs;
    // If the route argument 'hide' is true and the sidebar is visible, hide the sidebar.
    if (args.hide && ctx.sidebar.visible) {
      ctx.sidebar.toggleVisibility();
    }

    // Parse and store slice ID if provided
    if (args.sliceId !== undefined) {
      this.selectSliceId = parseInt(args.sliceId);
    }

    // Store event name if provided
    if (args.eventName !== undefined) {
      this.selectEventName = args.eventName;
    }
  }

  /**
   * After trace loading completes, automatically selects either:
   * - The specified slice by ID (direct selection)
   * - The first matching slice by name (fuzzy selection)
   * Scrolls to the selected item in the UI
   */
  async onTraceLoad(ctx: Trace) {
    // Direct slice selection by ID
    if (LynxRouterArgsPlugin.selectSliceId !== -1) {
      ctx.selection.selectSqlEvent(
        'slice',
        LynxRouterArgsPlugin.selectSliceId,
        {
          scrollToSelection: true,
        },
      );
      return;
    }

    // Fuzzy selection by event name
    if (LynxRouterArgsPlugin.selectEventName) {
      const result = await ctx.engine.query(`
        select
          id
        from slice
        where slice.name = '${LynxRouterArgsPlugin.selectEventName}';
      `);
      if (result.numRows() === 0) {
        return undefined;
      }

      const {id} = result.firstRow({
        id: NUM,
      });

      ctx.selection.selectSqlEvent('slice', id, {
        scrollToSelection: true,
      });
    }
  }
}
