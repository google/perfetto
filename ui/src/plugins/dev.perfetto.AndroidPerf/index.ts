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
  Plugin,
  PluginContext,
  PluginDescriptor,
} from '../../public';

class AndroidPerf implements Plugin {
  onActivate(ctx: PluginContext): void {
    const {viewer} = ctx;

    ctx.addCommand({
      id: 'dev.perfetto.AndroidPerf#BinderSystemServerIncoming',
      name: 'Run query: system_server incoming binder graph',
      callback: () => viewer.tabs.openQuery(
          `INCLUDE PERFETTO MODULE android.binder;
           SELECT * FROM android_binder_incoming_graph((SELECT upid FROM process WHERE name = 'system_server'))`,
          'system_server incoming binder graph'),
    });

    ctx.addCommand({
      id: 'dev.perfetto.AndroidPerf#BinderSystemServerOutgoing',
      name: 'Run query: system_server outgoing binder graph',
      callback: () => viewer.tabs.openQuery(
          `INCLUDE PERFETTO MODULE android.binder;
           SELECT * FROM android_binder_outgoing_graph((SELECT upid FROM process WHERE name = 'system_server'))`,
          'system_server outgoing binder graph'),
    });

    ctx.addCommand({
      id: 'dev.perfetto.AndroidPerf#MonitorContentionSystemServer',
      name: 'Run query: system_server monitor_contention graph',
      callback: () => viewer.tabs.openQuery(
          `INCLUDE PERFETTO MODULE android.monitor_contention;
           SELECT * FROM android_monitor_contention_graph((SELECT upid FROM process WHERE name = 'system_server'))`,
          'system_server monitor_contention graph'),
    });

    ctx.addCommand({
      id: 'dev.perfetto.AndroidPerf#BinderAll',
      name: 'Run query: all process binder graph',
      callback: () => viewer.tabs.openQuery(
          `INCLUDE PERFETTO MODULE android.binder;
           SELECT * FROM android_binder_graph(-1000, 1000, -1000, 1000)`,
          'all process binder graph'),
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'dev.perfetto.AndroidPerf',
  plugin: AndroidPerf,
};
