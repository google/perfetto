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
import {existsSync, createReadStream} from 'fs';

const UI_DIR = resolve(__dirname, '..');
const ROOT_DIR = resolve(UI_DIR, '..');

// Plugin to serve WASM files and static assets in dev mode
export function devStaticServePlugin(): Plugin {
  const WASM_DIR = resolve(UI_DIR, '../out/ui/ui/dist_version');
  const FONTS_DIR = resolve(UI_DIR, '../buildtools/typefaces');
  const crossOriginIsolation = process.env.CORP_ENABLED === '1';

  return {
    name: 'dev-static-serve',
    configureServer(server) {
      server.middlewares.use((req, res, next) => {
        // Add Cross-Origin Isolation headers if enabled
        if (crossOriginIsolation) {
          res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
          res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
        }

        // Serve test files from /test/
        if (req.url?.startsWith('/test/')) {
          const filePath = resolve(ROOT_DIR, req.url.slice(1));
          if (existsSync(filePath)) {
            createReadStream(filePath).pipe(res);
            return;
          }
        }

        // Serve WASM files from /wasm/
        if (req.url?.startsWith('/wasm/')) {
          const filename = req.url.replace('/wasm/', '');
          const filePath = resolve(WASM_DIR, filename);

          if (existsSync(filePath)) {
            res.setHeader('Content-Type', 'application/wasm');
            createReadStream(filePath).pipe(res);
            return;
          }
        }

        // Serve font files from /assets/assets/ (CSS relative path quirk)
        // Fonts are in buildtools/typefaces/
        if (req.url?.startsWith('/assets/assets/')) {
          const filename = req.url.replace('/assets/assets/', '');
          const filePath = resolve(FONTS_DIR, filename);

          if (existsSync(filePath)) {
            if (filename.endsWith('.woff2')) {
              res.setHeader('Content-Type', 'font/woff2');
            } else if (filename.endsWith('.woff')) {
              res.setHeader('Content-Type', 'font/woff');
            }
            createReadStream(filePath).pipe(res);
            return;
          }
        }

        next();
      });
    },
  };
}
