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
import {TraceFileStream} from '../../core/trace_stream';
import {STR} from '../../trace_processor/query_result';

// The result of analyzing a trace file.
export interface TraceAnalysisResult {
  format: string;
}

// Interface for analyzing trace files to extract metadata.
export interface TraceAnalyzer {
  analyze(
    file: File,
    onProgress: (progress: number) => void,
  ): Promise<TraceAnalysisResult>;
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

// Implementation of TraceAnalyzer using the WASM trace processor.
export class WasmTraceAnalyzer implements TraceAnalyzer {
  async analyze(
    file: File,
    onProgress: (progress: number) => void,
  ): Promise<TraceAnalysisResult> {
    using engine = new WasmEngineProxy(uuidv4());
    engine.resetTraceProcessor({
      tokenizeOnly: true,
      cropTrackEvents: false,
      ingestFtraceInRawTable: false,
      analyzeTraceProtoContent: false,
      ftraceDropUntilAllCpusValid: false,
      forceFullSort: false,
    });
    const stream = new TraceFileStream(file);
    for (;;) {
      const res = await stream.readChunk();
      onProgress(res.bytesRead / file.size);
      await engine.parse(res.data);
      if (res.eof) {
        await engine.notifyEof();
        break;
      }
    }
    const result = await engine.query(`
        SELECT
          parent.trace_type
        FROM __intrinsic_trace_file parent
        LEFT JOIN __intrinsic_trace_file child ON parent.id = child.parent_id
        WHERE child.id IS NULL
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

    return {
      format: mapTraceType(leafNodes[0]),
    };
  }
}
