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
import {
  ExtensionInitializationResult,
  initializeExtensions,
} from './extension_server';
import {ExtensionServer, ExtensionServersSchema} from './types';
import {renderExtensionServersSettings} from './extension_servers_settings';

export const DEFAULT_EXTENSION_SERVERS: ExtensionServer[] = [
  // TODO(lalitmn): populate with the default server here.
];

export default class ExtensionServersPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.ExtensionServers';

  static onActivate(ctx: AppImpl) {
    // Register the extension servers setting
    const setting = ctx.settings.register<ExtensionServer[]>({
      id: 'extensionServers',
      name: 'Extension servers',
      description:
        'Configure external extension servers that provide additional macros, SQL modules, and proto descriptors.',
      defaultValue: DEFAULT_EXTENSION_SERVERS,
      schema: ExtensionServersSchema,
      render: renderExtensionServersSettings,
    });

    // Initialize extension servers asynchronously
    initializeExtensions(setting.get()).then(
      (result: ExtensionInitializationResult) => {
        // Convert macros Map to Record and push to extraMacros
        ctx.addExtensionMacrosPromise(
          result.macrosPromise.then((macros) => Object.assign({}, ...macros)),
        );

        // Convert SQL modules Map to SqlPackage and push to extraSqlPackages
        ctx.addExtensionSqlPackagesPromise(
          result.sqlModulesPromise.then((modules) => [
            {
              // TODO(lalitm): DNS. This needs to be discussed before submitting.
              name: 'extension_servers',
              modules: Array.from(modules, ([name, sql]) => ({name, sql})),
            },
          ]),
        );

        // Extract proto descriptor strings and push to extraParsingDescriptors
        ctx.addExtensionParsingDescriptorsPromise(
          result.protoDescriptorsPromise,
        );
      },
    );
  }
}
