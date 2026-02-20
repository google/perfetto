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

// This file uses Vite's import.meta.glob to discover plugins at build time.
// It replaces the generated all_plugins.ts from the old build system.

import {PerfettoPlugin} from '../public/plugin';

type PluginModule = {default: typeof PerfettoPlugin};

// Eagerly import all plugin index.ts files at build time
const pluginModules = import.meta.glob<PluginModule>(
  '../plugins/*/index.ts',
  {eager: true},
);

// Extract the default exports (plugin classes) into an array
const plugins: Array<typeof PerfettoPlugin> = Object.values(pluginModules).map(
  (mod) => mod.default,
);

export default plugins;
