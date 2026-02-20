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

import {Plugin} from 'vite';
import {resolve, dirname} from 'path';
import {existsSync, readFileSync} from 'fs';

const UI_DIR = resolve(__dirname, '..');

// Plugin to wrap Emscripten WASM modules as ESM
export function wasmModulesPlugin(): Plugin {
  const GEN_DIR = resolve(UI_DIR, '../out/ui/ui/tsc/gen');

  // Emscripten modules that need wrapping
  const WASM_MODULES = ['proto_utils', 'traceconv', 'trace_processor_memory64'];

  return {
    name: 'wasm-modules',
    enforce: 'pre',

    resolveId(source, importer) {
      // Check for ../gen/<wasm_module> imports
      for (const mod of WASM_MODULES) {
        if (source === `../gen/${mod}`) {
          return `\0virtual:wasm:${mod}`;
        }
        // Also handle when the path has been partially resolved
        if (source.endsWith(`/gen/${mod}`) || source.endsWith(`/gen/${mod}.js`)) {
          return `\0virtual:wasm:${mod}`;
        }
      }
      return null;
    },

    load(id) {
      if (id.startsWith('\0virtual:wasm:')) {
        const modName = id.replace('\0virtual:wasm:', '');
        const jsPath = resolve(GEN_DIR, `${modName}.js`);

        if (!existsSync(jsPath)) {
          throw new Error(
            `WASM module not found: ${jsPath}. Run the build first.`,
          );
        }

        const content = readFileSync(jsPath, 'utf-8');
        // Wrap the IIFE as an ESM default export
        // The Emscripten output is: var foo_wasm = (() => { ... })();
        // We transform to: export default foo_wasm;
        const varName = `${modName}_wasm`;
        return content + `\nexport default ${varName};`;
      }
      return null;
    },
  };
}
