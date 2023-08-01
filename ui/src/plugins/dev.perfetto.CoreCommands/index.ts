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

class CoreCommands implements TracePlugin {
  static migrate(_initialState: unknown): {} {
    return {};
  }

  private viewer: Viewer;

  constructor(_store: Store<{}>, _engine: EngineProxy, viewer: Viewer) {
    this.viewer = viewer;
  }

  dispose(): void {
    // No-op
  }

  commands(): Command[] {
    return [{
      id: 'dev.perfetto.CoreCommands.ToggleLeftSidebar',
      name: 'Toggle left sidebar',
      callback: () => {
        if (this.viewer.sidebar.isVisible()) {
          this.viewer.sidebar.hide();
        } else {
          this.viewer.sidebar.show();
        }
      },
    }];
  }
}

function activate(ctx: PluginContext) {
  ctx.registerTracePluginFactory(CoreCommands);
}

export const plugin = {
  pluginId: 'dev.perfetto.CoreCommands',
  activate,
};
