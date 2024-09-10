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

import {Trace} from '../../public/trace';
import {App} from '../../public/app';
import {PerfettoPlugin, PluginDescriptor} from '../../public/plugin';

declare global {
  interface Window {
    ctx: App | Trace | undefined;
  }
}

class BookmarkletApi implements PerfettoPlugin {
  private pluginCtx?: App;

  onActivate(pluginCtx: App): void {
    this.pluginCtx = pluginCtx;
    window.ctx = pluginCtx;
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    window.ctx = ctx;
  }

  async onTraceUnload(_: Trace): Promise<void> {
    window.ctx = this.pluginCtx;
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'dev.perfetto.BookmarkletApi',
  plugin: BookmarkletApi,
};
