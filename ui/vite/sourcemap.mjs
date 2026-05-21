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

import {SourceMapConsumer, SourceMapGenerator} from 'source-map';

// Strips Rollup's full source map down to one mapping per generated line and
// embeds it inline into each _bundle.js under self.__SOURCEMAPS[fileName] for
// runtime error reporting. Skipped when source maps are disabled.
//
// |sourceReplacements| is an array of [from, to] pairs applied to each source
// path before emission, to strip build-time prefixes (e.g. the out/ symlink).
export function pluginEmbedMinimalSourceMap({sourceReplacements = []} = {}) {
  const cleanSourcePath = (source) => {
    let cleaned = source;
    for (const [from, to] of sourceReplacements) {
      cleaned = cleaned.replace(from, to);
    }
    return cleaned;
  };
  return {
    name: 'perfetto:embed-minimal-sourcemap',
    async generateBundle(_options, bundle) {
      for (const chunk of Object.values(bundle)) {
        if (!chunk.fileName || !chunk.fileName.endsWith('.js') || !chunk.map) {
          continue;
        }
        try {
          const consumer = await new SourceMapConsumer(chunk.map);
          const generator = new SourceMapGenerator({file: chunk.map.file});
          const seenLines = new Set();
          consumer.eachMapping((mapping) => {
            if (!mapping.source) return;
            if (seenLines.has(mapping.generatedLine)) return;
            seenLines.add(mapping.generatedLine);
            generator.addMapping({
              generated: {line: mapping.generatedLine, column: 0},
              original: {
                line: mapping.originalLine,
                column: mapping.originalColumn,
              },
              source: cleanSourcePath(mapping.source),
            });
          });
          consumer.destroy();
          const minimalMap = JSON.parse(generator.toString());
          delete minimalMap.sourcesContent;
          delete minimalMap.names;
          chunk.code +=
            `\n;(self.__SOURCEMAPS=self.__SOURCEMAPS||{})` +
            `['${chunk.fileName}']=${JSON.stringify(minimalMap)};`;
        } catch (err) {
          console.error(
            `Error creating minimal source map for ${chunk.fileName}:`,
            err.message,
          );
        }
      }
    },
  };
}
