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

import './styles.scss';
import type {App} from '../../public/app';
import type {PerfettoPlugin} from '../../public/plugin';
import type {RouteArgs} from '../../public/route_schema';
import type {Trace} from '../../public/trace';
import {LiveController} from './controller';
import {LIVE_TRACE_ARGS, PLUGIN_ID} from './opener';
import {LiveSession} from './session';

/** Shows a live server's stream as a growing trace; see README.md. */
// The plugin loader imports plugins by default export.
// tslint:disable-next-line:no-default-export
export default class LiveStreamPlugin implements PerfettoPlugin {
  static readonly id = PLUGIN_ID;
  static readonly description =
    'Shows slices and counters streamed by a live server as a trace that ' +
    'grows while you watch. Active only with the route argument ' +
    `${PLUGIN_ID}:target, which the server sets.`;

  // Set while the page shows a live stream.
  private static controller?: LiveController;

  static onActivate(app: App, args: RouteArgs): void {
    const target = args.target;
    if (typeof target !== 'string') return;
    LiveStreamPlugin.controller = new LiveController(app);
    LiveStreamPlugin.controller.open(target);
  }

  async onTraceLoad(
    trace: Trace,
    args?: {[key: string]: unknown},
  ): Promise<void> {
    const controller = LiveStreamPlugin.controller;
    if (controller === undefined) return;
    const parsed = LIVE_TRACE_ARGS.safeParse(args);
    if (!parsed.success) {
      controller.showOtherTrace();
      return;
    }

    const session = new LiveSession(trace, parsed.data, controller);
    trace.trash.use(session);
    await session.init();
  }
}
