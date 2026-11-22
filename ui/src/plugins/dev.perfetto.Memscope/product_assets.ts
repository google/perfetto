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

// The Memscope product's shared SQL package and record config. Both live in
// products/memscope/ and are also embedded in the memscope CLI, so the UI and
// the CLI run the same queries and record the same data.

import {ensureExists} from '../../base/assert';
import type {SqlPackage} from '../../public/extra_sql_packages';
import {PRODUCT_CONFIGS, PRODUCT_SQL_PACKAGES} from '../../gen/products_sql';

// The `memscope` PerfettoSQL package (products/memscope/sql/memscope).
export const MEMSCOPE_SQL_PACKAGE: SqlPackage = ensureExists(
  PRODUCT_SQL_PACKAGES.find((p) => p.name === 'memscope'),
);

export interface RecordConfigArgs {
  readonly app: string;
  // 0 records until the session is stopped.
  readonly durationMs: number;
  // Omit for no Java heap dumps.
  readonly javaDumps?: {readonly phaseMs: number; readonly intervalMs: number};
}

function dropBlock(cfg: string, name: string): string {
  const begin = `# BEGIN ${name}\n`;
  const end = `# END ${name}\n`;
  const b = cfg.indexOf(begin);
  const e = cfg.indexOf(end);
  if (b < 0 || e < b) throw new Error(`record.pbtx has no ${name} block`);
  return cfg.slice(0, b) + cfg.slice(e + end.length);
}

// Fills products/memscope/configs/record.pbtx, as `memscope record` does.
export function buildRecordConfigTxt(args: RecordConfigArgs): string {
  let cfg = ensureExists(PRODUCT_CONFIGS['memscope/record.pbtx']);
  if (args.javaDumps === undefined) {
    cfg = dropBlock(cfg, 'java_hprof');
  } else {
    cfg = cfg
      .replaceAll('{{DUMP_PHASE_MS}}', `${args.javaDumps.phaseMs}`)
      .replaceAll('{{DUMP_INTERVAL_MS}}', `${args.javaDumps.intervalMs}`);
  }
  return cfg
    .replaceAll('{{APP}}', args.app)
    .replaceAll('{{DURATION_MS}}', `${args.durationMs}`);
}
