// Copyright (C) 2023 The Android Open Source Project
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

import {TraceFile, TraceFileAnalyzed} from './multi_trace_types';
import {uuidv4} from '../../base/uuid';
import {TraceAnalyzer} from './trace_analyzer';

function getErrorMessage(e: unknown): string {
  const err = e instanceof Error ? e.message : `${e}`;
  if (err.includes('(ERR:fmt)')) {
    return `The file opened doesn't look like a Perfetto trace or any other supported trace format.`;
  }
  return err;
}

// The possible error states for the modal, used to show a helpful message
// to the user and disable the "Open Traces" button.
export type LoadingError = 'NO_TRACES' | 'ANALYZING' | 'TRACE_ERROR';

/**
 * The controller for the multi-trace modal.
 * This class manages the state of the traces and their analysis.
 */
export class MultiTraceController {
  private _traces: TraceFile[] = [];
  private traceAnalyzer: TraceAnalyzer;
  private onStateChanged: () => void;
  private onAnalysisStarted?: (traceUuid: string) => void;
  private onAnalysisCompleted?: (traceUuid: string) => void;

  constructor(
    traceAnalyzer: TraceAnalyzer,
    onStateChanged: () => void,
    onAnalysisStarted?: (traceUuid: string) => void,
    onAnalysisCompleted?: (traceUuid: string) => void,
  ) {
    this.traceAnalyzer = traceAnalyzer;
    this.onStateChanged = onStateChanged;
    this.onAnalysisStarted = onAnalysisStarted;
    this.onAnalysisCompleted = onAnalysisCompleted;
  }

  // Test-only method to set traces directly
  setTracesForTesting(traces: TraceFile[]) {
    this._traces = [...traces];
  }

  get traces(): ReadonlyArray<TraceFile> {
    return this._traces;
  }

  get isOpeningAllowed(): boolean {
    return this.getLoadingError() === undefined;
  }

  getLoadingError(): LoadingError | undefined {
    if (this.traces.length === 0) {
      return 'NO_TRACES';
    }
    if (this.isAnalyzing()) {
      return 'ANALYZING';
    }
    if (this.hasErrors()) {
      return 'TRACE_ERROR';
    }
    return undefined;
  }

  addFiles(files: ReadonlyArray<File>) {
    for (const file of files) {
      const trace: TraceFile = {
        uuid: uuidv4(),
        file: file,
        status: 'not-analyzed',
      };
      this._traces.push(trace);
      this.analyzeTrace(trace);
    }
    this.onStateChanged();
  }

  removeTrace(uuid: string) {
    const index = this._traces.findIndex((t) => t.uuid === uuid);
    if (index !== -1) {
      this._traces.splice(index, 1);
      this.onStateChanged();
    }
  }

  isAnalyzing(): boolean {
    return this.traces.some((t) => t.status === 'analyzing');
  }

  private hasErrors(): boolean {
    return this.traces.some((t) => t.status === 'error');
  }

  private async analyzeTrace(trace: TraceFile) {
    const index = this._traces.findIndex((t) => t.uuid === trace.uuid);
    if (index === -1) return;

    try {
      this._traces[index] = {
        ...trace,
        status: 'analyzing',
        progress: 0,
      };
      this.onStateChanged();
      this.onAnalysisStarted?.(trace.uuid);

      const result = await this.traceAnalyzer.analyze(
        trace.file,
        (progress) => {
          if (this._traces[index]?.status === 'analyzing') {
            this._traces[index] = {
              ...this._traces[index],
              progress,
            };
            this.onStateChanged();
          }
        },
      );

      const analyzedTrace: TraceFileAnalyzed = {
        ...trace,
        status: 'analyzed',
        format: result.format,
      };
      this._traces[index] = analyzedTrace;
      this.onStateChanged();
      this.onAnalysisCompleted?.(trace.uuid);
    } catch (e) {
      this._traces[index] = {
        ...trace,
        status: 'error',
        error: getErrorMessage(e),
      };
      this.onStateChanged();
      this.onAnalysisCompleted?.(trace.uuid);
    }
  }
}
