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
import {resolve} from 'path';
import {execSync} from 'child_process';
import {existsSync, mkdirSync, readFileSync, statSync} from 'fs';

const UI_DIR = resolve(__dirname, '..');
const ROOT_DIR = resolve(UI_DIR, '..');

// Plugin to compile .proto files to ESM using pbjs
export function protobufPlugin(): Plugin {
  const PROTO_FILES = [
    'protos/perfetto/ipc/consumer_port.proto',
    'protos/perfetto/ipc/wire_protocol.proto',
    'protos/perfetto/trace/perfetto/perfetto_metatrace.proto',
    'protos/perfetto/perfetto_sql/structured_query.proto',
    'protos/perfetto/trace_processor/trace_processor.proto',
  ];

  const cacheDir = resolve(UI_DIR, 'node_modules/.vite-proto-cache');
  const cachedJs = resolve(cacheDir, 'protos.mjs');

  // Get the newest mtime of all proto files
  function getNewestProtoMtime(): number {
    return Math.max(
      ...PROTO_FILES.map((f) => statSync(resolve(ROOT_DIR, f)).mtimeMs),
    );
  }

  // Check if cache is valid (exists and newer than all proto files)
  function isCacheValid(): boolean {
    if (!existsSync(cachedJs)) return false;
    const cacheMtime = statSync(cachedJs).mtimeMs;
    const newestProto = getNewestProtoMtime();
    return cacheMtime > newestProto;
  }

  return {
    name: 'protobuf',
    enforce: 'pre',

    resolveId(source, importer) {
      // Intercept imports to ../gen/protos
      if (source === '../gen/protos' && importer) {
        return '\0virtual:protos';
      }
      return null;
    },

    load(id) {
      if (id === '\0virtual:protos') {
        // Check cache first (with mtime validation)
        if (isCacheValid()) {
          return readFileSync(cachedJs, 'utf-8');
        }

        // Generate using pbjs
        if (!existsSync(cacheDir)) {
          mkdirSync(cacheDir, {recursive: true});
        }

        const protoArgs = PROTO_FILES.map((p) => resolve(ROOT_DIR, p)).join(
          ' ',
        );
        const pbjsPath = resolve(UI_DIR, 'node_modules/.bin/pbjs');

        try {
          // Generate ESM output
          execSync(
            `${pbjsPath} --no-beautify --force-number --no-delimited --no-verify ` +
              `-t static-module -w es6 -p ${ROOT_DIR} -o ${cachedJs} ${protoArgs}`,
            {stdio: 'pipe'},
          );

          return readFileSync(cachedJs, 'utf-8');
        } catch (e) {
          console.error('Failed to generate protos:', e);
          throw e;
        }
      }
      return null;
    },
  };
}
