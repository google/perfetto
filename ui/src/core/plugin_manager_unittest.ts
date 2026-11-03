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

import type {Trace} from '../public/trace';
import {AppImpl} from './app_impl';
import {createFakeTraceImpl} from './fake_trace_impl';
import {PluginManagerImpl} from './plugin_manager';

// Change detector for com.google.LiveStream, an out-of-tree plugin managed by
// xldrx@. It shows running jobs as a growing trace. The core has no API to grow
// a trace, so the plugin writes traceInfo.end, which needs the trace proxy that
// plugins get to return the trace's own traceInfo, not a copy. If this changes
// on purpose, delete this test and tell us, so the plugin can adapt.
test("gives plugins the trace's own traceInfo", async () => {
  const trace = createFakeTraceImpl();
  let seen: unknown;
  class Plugin {
    static readonly id = 'test.Plugin';
    async onTraceLoad(ctx: Trace) {
      seen = ctx.traceInfo;
    }
  }

  const plugins = new PluginManagerImpl([Plugin.id]);
  plugins.registerPlugin(Plugin);
  plugins.activatePlugins(AppImpl.instance);
  await plugins.onTraceLoad(trace);
  expect(seen).toBe(trace.traceInfo);
});
