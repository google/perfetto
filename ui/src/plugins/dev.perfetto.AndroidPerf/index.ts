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

import {
  Command,
  EngineProxy,
  PluginContext,
  Store,
  TracePlugin,
  Viewer,
} from '../../public';

interface State {}

class AndroidPerf implements TracePlugin {
  static migrate(_initialState: unknown): State {
    return {};
  }

  private viewer: Viewer;

  constructor(_store: Store<State>, _engine: EngineProxy, viewer: Viewer) {
    this.viewer = viewer;
  }

  dispose(): void {}

  commands(): Command[] {
    return [
      {
        id: 'dev.perfetto.AndroidPerf#BinderSystemServerIncoming',
        name: 'Run query: system_server incoming binder graph',
        callback: () => this.viewer.tabs.openQuery(
            `SELECT IMPORT('android.binder');
             SELECT * FROM android_binder_incoming_graph((SELECT upid FROM process WHERE name = 'system_server'))`,
            'system_server incoming binder graph'),
      },
      {
        id: 'dev.perfetto.AndroidPerf#BinderSystemServerOutgoing',
        name: 'Run query: system_server outgoing binder graph',
        callback: () => this.viewer.tabs.openQuery(
            `SELECT IMPORT('android.binder');
             SELECT * FROM android_binder_outgoing_graph((SELECT upid FROM process WHERE name = 'system_server'))`,
            'system_server outgoing binder graph'),
      },
      {
        id: 'dev.perfetto.AndroidPerf#MonitorContentionSystemServer',
        name: 'Run query: system_server monitor_contention graph',
        callback: () => this.viewer.tabs.openQuery(
            `SELECT IMPORT('android.monitor_contention');
             SELECT * FROM android_monitor_contention_graph((SELECT upid FROM process WHERE name = 'system_server'))`,
            'system_server monitor_contention graph'),
      },
      {
        id: 'dev.perfetto.AndroidPerf#BinderAll',
        name: 'Run query: all process binder graph',
        callback: () => this.viewer.tabs.openQuery(
            `SELECT IMPORT('android.binder');
             SELECT * FROM android_binder_graph(-1000, 1000, -1000, 1000)`,
            'all process binder graph'),
      },
    ];
  }
}

export const plugin = {
  pluginId: 'dev.perfetto.AndroidPerf',
  activate: (ctx: PluginContext) => {
    ctx.registerTracePluginFactory(AndroidPerf);
  },
};
