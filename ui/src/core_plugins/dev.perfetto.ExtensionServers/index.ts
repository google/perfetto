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

import {AppImpl} from '../../core/app_impl';
import {CommandInvocation} from '../../core/command_manager';
import {PerfettoPlugin} from '../../public/plugin';
import {SqlPackage} from '../../public/extra_sql_packages';
import {initializeExtensions} from './extension_server';
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
    initializeExtensions(setting.get()).then((result) => {
      // Convert macros Map to Record and push to extraMacros
      result.macrosPromise.then((macros) => {
        const macrosRecord: Record<string, CommandInvocation[]> = {};
        for (const [name, commands] of macros) {
          macrosRecord[name] = commands;
        }
        ctx.extraMacros.push(macrosRecord);
        ctx.extensionMacrosDeferred.resolve();
      });

      // Convert SQL modules Map to SqlPackage and push to extraSqlPackages
      result.sqlModulesPromise.then((modules) => {
        const sqlPackage: SqlPackage = {
          name: 'extension-servers',
          modules: Array.from(modules, ([name, sql]) => ({name, sql})),
        };
        ctx.extraSqlPackages.push(sqlPackage);
        ctx.extensionSqlModulesDeferred.resolve();
      });

      // Extract proto descriptor strings and push to extraParsingDescriptors
      result.protoDescriptorsPromise.then((descriptors) => {
        for (const [_, descriptor] of descriptors) {
          ctx.extraParsingDescriptors.push(descriptor.descriptor);
        }
        ctx.extensionProtoDescriptorsDeferred.resolve();
      });
    });
  }
}
