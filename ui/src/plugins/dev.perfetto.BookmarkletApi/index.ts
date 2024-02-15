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

import {
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';

declare global {
  interface Window {
    ctx: PluginContext | PluginContextTrace | undefined;
  }
}

class BookmarkletApi implements Plugin {
  private pluginCtx?: PluginContext;

  onActivate(pluginCtx: PluginContext): void {
    this.pluginCtx = pluginCtx;
    window.ctx = pluginCtx;
  }

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    window.ctx = ctx;
  }

  async onTraceUnload(_: PluginContextTrace): Promise<void> {
    window.ctx = this.pluginCtx;
  }

  onDeactivate(_: PluginContext): void {
    window.ctx = undefined;
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'dev.perfetto.BookmarkletApi',
  plugin: BookmarkletApi,
};
