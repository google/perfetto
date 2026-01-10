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

import {AppImpl} from '../../core/app_impl';
import {PerfettoPlugin} from '../../public/plugin';
import {initializeExtensions} from './extension_server';
import {ExtensionServer, extensionServersSchema} from './types';
import {renderExtensionServersSettings} from './extension_servers_settings';

export const DEFAULT_EXTENSION_SERVERS: ExtensionServer[] = [
  // TODO(lalitmn): populate with the default server here.
];

export default class ExtensionServersPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.ExtensionServers';

  static onActivate(ctx: AppImpl) {
    // Register the extension servers setting
    const setting = ctx.settings.register<ExtensionServer[]>({
      id: 'dev.perfetto.ExtensionServers#extensionServers',
      name: 'Extension Servers',
      description:
        'Configure servers that provide additional macros, SQL modules, and proto descriptors.',
      defaultValue: DEFAULT_EXTENSION_SERVERS,
      schema: extensionServersSchema,
      render: renderExtensionServersSettings,
    });
    initializeExtensions(ctx, setting.get());
  }
}
