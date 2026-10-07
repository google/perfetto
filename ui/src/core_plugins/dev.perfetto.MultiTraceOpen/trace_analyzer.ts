// Copyright (C) 2025 The Android Open Source Project
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

import {WasmEngineProxy} from '../../trace_processor/wasm_engine_proxy';
import {uuidv4} from '../../base/uuid';
import {getErrorMessage} from '../../base/errors';
import {
  TraceFileStream,
  TraceMultipleFilesStream,
} from '../../core/trace_stream';
import {NUM, STR, STR_NULL} from '../../trace_processor/query_result';
import type {TraceStream} from '../../public/stream';
import {BUILTIN_CLOCKS, type FileAnalysis} from './multi_trace_types';

export interface AlignmentVerdict {
  // True if there are no dropped events and no manifest validation error.
  readonly ok: boolean;
  // Events that would be dropped because their clock domain can't be related
  // to the shared timeline (clock_sync_unrelatable_clock_domains et al).
  readonly droppedEvents: number;
  // Set if the manifest itself is invalid (e.g. is.file names an unknown file).
  readonly validationError?: string;
}

export interface TraceAnalyzer {
  // Per-file analysis: format + best-effort clock/machine shape.
  analyze(
    file: File,
    onProgress: (progress: number) => void,
  ): Promise<FileAnalysis>;

  // Whole-set dry-run: build the manifest+files archive and report alignment.
  analyzeMergedAlignment(files: ReadonlyArray<File>): Promise<AlignmentVerdict>;
}

// Maps internal trace type names to user-friendly format names.
function mapTraceType(rawType: string): string {
  switch (rawType) {
    case 'proto':
      return 'Perfetto';
    default:
      return rawType;
  }
}

async function parseStream(
  engine: WasmEngineProxy,
  stream: TraceStream,
  onProgress?: (bytesRead: number) => void,
): Promise<void> {
  for (;;) {
    const res = await stream.readChunk();
    onProgress?.(res.bytesRead);
    await engine.parse(res.data);
    if (res.eof) {
      await engine.notifyEof();
      return;
    }
  }
}

// Tokenization populates trace_type, machine and clock_snapshot and records the
// clock-sync drop stats (conversion runs in the tokenizer), so it suffices here.
function newTokenizeOnlyEngine(): WasmEngineProxy {
  const engine = new WasmEngineProxy(uuidv4());
  engine.resetTraceProcessor({
    tokenizeOnly: true,
    cropTrackEvents: false,
    ingestFtraceInRawTable: false,
    analyzeTraceProtoContent: false,
    ftraceDropUntilAllCpusValid: false,
    forceFullSort: false,
  });
  return engine;
}

// Best-effort clock/machine signals; on query failure the fields stay undefined
// and the UI degrades to showing all controls.
async function queryFileSignals(
  engine: WasmEngineProxy,
): Promise<Partial<FileAnalysis>> {
  const out: {
    singleClock?: boolean;
    privateClockOnly?: boolean;
    builtinClockIds?: number[];
    singleMachine?: boolean;
    embeddedMachineIds?: number[];
  } = {};

  try {
    const res = await engine.query(
      `SELECT raw_id FROM machine ORDER BY raw_id`,
    );
    const it = res.iter({raw_id: NUM});
    const ids: number[] = [];
    for (; it.valid(); it.next()) {
      ids.push(it.raw_id);
    }
    if (ids.length > 0) {
      out.embeddedMachineIds = ids;
      // A trace is single-machine if every machine seen is the host (raw_id 0).
      out.singleMachine = ids.every((id) => id === 0);
    }
  } catch {
    // machine table unavailable in tokenize-only on this build.
  }

  try {
    const builtinIds = BUILTIN_CLOCKS.map((c) => c.id).join(', ');
    const res = await engine.query(`
      SELECT
        (SELECT COUNT(DISTINCT clock_id) FROM clock_snapshot) AS distinctClocks,
        (SELECT GROUP_CONCAT(DISTINCT clock_id) FROM clock_snapshot
           WHERE clock_id IN (${builtinIds})) AS builtinClockIds
    `);
    const it = res.iter({
      distinctClocks: NUM,
      builtinClockIds: STR_NULL,
    });
    if (it.valid()) {
      // ClockSnapshots tie multiple domains; single-clock traces emit <= 1.
      out.singleClock = it.distinctClocks <= 1;
      // No snapshot at all => only the private trace-file clock (e.g. JSON).
      out.privateClockOnly = it.distinctClocks === 0;
      out.builtinClockIds = parseClockIds(it.builtinClockIds);
    }
  } catch {
    // clock_snapshot unavailable in tokenize-only on this build.
  }

  return out;
}

function parseClockIds(s: string | null): number[] {
  if (s === null || s.length === 0) {
    return [];
  }
  return s.split(',').map(Number);
}

export class WasmTraceAnalyzer implements TraceAnalyzer {
  async analyze(
    file: File,
    onProgress: (progress: number) => void,
  ): Promise<FileAnalysis> {
    using engine = newTokenizeOnlyEngine();
    await parseStream(engine, new TraceFileStream(file), (n) =>
      onProgress(n / file.size),
    );
    const result = await engine.query(`
        SELECT trace_type
        FROM __intrinsic_trace_file
        WHERE is_container = 0
      `);
    const it = result.iter({trace_type: STR});
    const leafNodes = [];
    for (; it.valid(); it.next()) {
      leafNodes.push(it.trace_type);
    }
    if (leafNodes.length > 1) {
      throw new Error(
        'This trace contains multiple sub-traces, which is not supported ' +
          'because recursive synchronization is tricky. Please open each ' +
          'sub-trace individually.',
      );
    }
    if (leafNodes.length === 0) {
      throw new Error('Could not determine trace type');
    }

    const signals = await queryFileSignals(engine);
    return {
      format: mapTraceType(leafNodes[0]),
      ...signals,
    };
  }

  async analyzeMergedAlignment(
    files: ReadonlyArray<File>,
  ): Promise<AlignmentVerdict> {
    using engine = newTokenizeOnlyEngine();
    try {
      await parseStream(engine, new TraceMultipleFilesStream(files));
    } catch (e) {
      // A manifest validation error (or unreadable input) surfaces here.
      return {ok: false, droppedEvents: 0, validationError: getErrorMessage(e)};
    }

    try {
      const res = await engine.query(`
        SELECT COALESCE(SUM(value), 0) AS dropped
        FROM stats
        WHERE name IN (
          'clock_sync_unrelatable_clock_domains',
          'clock_sync_failure_no_path',
          'trace_sorter_negative_timestamp_dropped'
        )
      `);
      const it = res.iter({dropped: NUM});
      const dropped = it.valid() ? it.dropped : 0;
      return {ok: dropped === 0, droppedEvents: dropped};
    } catch (e) {
      return {ok: false, droppedEvents: 0, validationError: getErrorMessage(e)};
    }
  }
}
