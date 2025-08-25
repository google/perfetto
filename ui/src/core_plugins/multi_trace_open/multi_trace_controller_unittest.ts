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

import {assertExists} from '../../base/logging';
import {MultiTraceController} from './multi_trace_controller';
import {
  TraceFileAnalyzed,
  TraceFileAnalyzing,
  TraceFileError,
} from './multi_trace_types';
import {TraceAnalysisResult, TraceAnalyzer} from './trace_analyzer';

// Helper to create a mock TraceFileAnalyzed object for tests
function createMockAnalyzedTrace(
  uuid: string,
  format: string = 'Perfetto',
): TraceFileAnalyzed {
  return {
    uuid,
    file: new File([], `${uuid}.pftrace`),
    status: 'analyzed',
    format,
  };
}

// Helper to create a mock TraceFileAnalyzing object for tests
function createMockAnalyzingTrace(
  uuid: string,
  progress: number = 0.5,
): TraceFileAnalyzing {
  return {
    uuid,
    file: new File([], `${uuid}.pftrace`),
    status: 'analyzing',
    progress,
  };
}

// Helper to create a mock TraceFileError object for tests
function createMockErrorTrace(uuid: string, error: string): TraceFileError {
  return {
    uuid,
    file: new File([], `${uuid}.pftrace`),
    status: 'error',
    error,
  };
}

// A fake TraceAnalyzer for testing purposes.
class FakeTraceAnalyzer implements TraceAnalyzer {
  private results = new Map<string, TraceAnalysisResult>();
  private errors = new Map<string, Error>();

  setResult(fileName: string, result: TraceAnalysisResult) {
    this.results.set(fileName, result);
  }

  setError(fileName: string, error: Error) {
    this.errors.set(fileName, error);
  }

  async analyze(
    file: File,
    onProgress: (progress: number) => void,
  ): Promise<TraceAnalysisResult> {
    // Simulate progress
    onProgress(0.5);
    onProgress(1.0);

    if (this.errors.has(file.name)) {
      throw new Error(assertExists(this.errors.get(file.name)?.message));
    }
    const result = this.results.get(file.name);
    if (result) {
      return result;
    }
    throw new Error(`No mock result set for ${file.name}`);
  }
}

// Helper to create a File object for tests
function createMockFile(name: string): File {
  return new File([], name);
}

describe('MultiTraceController', () => {
  let controller: MultiTraceController;
  let fakeAnalyzer: FakeTraceAnalyzer;
  let onStateChanged: jest.Mock;
  let onAnalysisStarted: jest.Mock;
  let onAnalysisCompleted: jest.Mock;

  beforeEach(() => {
    fakeAnalyzer = new FakeTraceAnalyzer();
    onStateChanged = jest.fn();
    onAnalysisStarted = jest.fn();
    onAnalysisCompleted = jest.fn();
    controller = new MultiTraceController(
      fakeAnalyzer,
      onStateChanged,
      onAnalysisStarted,
      onAnalysisCompleted,
    );
  });

  it('should initialize with no traces', () => {
    expect(controller.traces).toHaveLength(0);
    expect(controller.isOpeningAllowed).toBe(false);
  });

  describe('addFiles', () => {
    it('should add files and trigger analysis', async () => {
      const file1 = createMockFile('trace1.pftrace');
      const file2 = createMockFile('trace2.pftrace');
      fakeAnalyzer.setResult(file1.name, {format: 'Perfetto'});
      fakeAnalyzer.setResult(file2.name, {format: 'Perfetto'});

      controller.addFiles([file1, file2]);

      expect(controller.traces).toHaveLength(2);
      // Analysis starts immediately, so status should be 'analyzing'
      expect(controller.traces[0].status).toBe('analyzing');
      expect(controller.traces[1].status).toBe('analyzing');
      expect(onStateChanged).toHaveBeenCalled();

      // Wait for analysis to complete
      await new Promise<void>((resolve) => {
        let completedCount = 0;
        onAnalysisCompleted.mockImplementation(() => {
          completedCount++;
          if (completedCount === 2) resolve();
        });
      });

      // Now traces should be analyzed
      expect(controller.traces[0].status).toBe('analyzed');
      expect(controller.traces[1].status).toBe('analyzed');
    });

    it('should handle format error messages', async () => {
      const file = createMockFile('bad_format.pftrace');
      fakeAnalyzer.setError(
        file.name,
        new Error('Something (ERR:fmt) happened'),
      );

      controller.addFiles([file]);

      // Wait for the async analysis to complete
      await new Promise<void>((resolve) => {
        onAnalysisCompleted.mockImplementation(() => resolve());
      });

      const trace = controller.traces[0];
      expect(trace.status).toBe('error');
      if (trace.status === 'error') {
        expect(trace.error).toContain("doesn't look like a Perfetto trace");
      }
    });
  });

  describe('removeTrace', () => {
    it('should remove a trace by UUID', () => {
      const trace1 = createMockAnalyzedTrace('uuid1');
      const trace2 = createMockAnalyzedTrace('uuid2');
      controller.setTracesForTesting([trace1, trace2]);

      controller.removeTrace('uuid1');

      expect(controller.traces).toHaveLength(1);
      expect(controller.traces[0].uuid).toBe('uuid2');
      expect(onStateChanged).toHaveBeenCalled();
    });

    it('should handle removing non-existent trace', () => {
      const trace = createMockAnalyzedTrace('uuid1');
      controller.setTracesForTesting([trace]);

      controller.removeTrace('non-existent-uuid');

      expect(controller.traces).toHaveLength(1);
    });
  });

  describe('isAnalyzing', () => {
    it('should return true when traces are being analyzed', () => {
      const trace1 = createMockAnalyzedTrace('uuid1');
      const trace2 = createMockAnalyzingTrace('uuid2', 0.5);
      controller.setTracesForTesting([trace1, trace2]);

      expect(controller.isAnalyzing()).toBe(true);
    });

    it('should return false when no traces are analyzing', () => {
      const trace1 = createMockAnalyzedTrace('uuid1');
      const trace2 = createMockAnalyzedTrace('uuid2');
      controller.setTracesForTesting([trace1, trace2]);

      expect(controller.isAnalyzing()).toBe(false);
    });

    it('should return false with no traces', () => {
      expect(controller.isAnalyzing()).toBe(false);
    });
  });

  describe('getLoadingError', () => {
    it('returns NO_TRACES when no traces are loaded', () => {
      expect(controller.getLoadingError()).toBe('NO_TRACES');
    });

    it('returns ANALYZING when traces are being analyzed', () => {
      const trace1 = createMockAnalyzedTrace('uuid1');
      const trace2 = createMockAnalyzingTrace('uuid2');
      controller.setTracesForTesting([trace1, trace2]);

      expect(controller.getLoadingError()).toBe('ANALYZING');
    });

    it('returns TRACE_ERROR when a trace has an error', () => {
      const trace1 = createMockAnalyzedTrace('uuid1');
      const trace2 = createMockErrorTrace('uuid2', 'Analysis failed');
      controller.setTracesForTesting([trace1, trace2]);

      expect(controller.getLoadingError()).toBe('TRACE_ERROR');
    });

    it('returns undefined when all traces are successfully analyzed', () => {
      const trace1 = createMockAnalyzedTrace('uuid1');
      const trace2 = createMockAnalyzedTrace('uuid2');
      controller.setTracesForTesting([trace1, trace2]);

      expect(controller.getLoadingError()).toBeUndefined();
      expect(controller.isOpeningAllowed).toBe(true);
    });
  });

  describe('async analysis behavior', () => {
    it('should complete analysis and update trace status', async () => {
      const file = createMockFile('trace.pftrace');
      fakeAnalyzer.setResult(file.name, {format: 'Perfetto'});

      controller.addFiles([file]);
      // Analysis starts immediately, so status should be 'analyzing'
      expect(controller.traces[0].status).toBe('analyzing');

      // Wait for async analysis to complete
      await new Promise<void>((resolve) => {
        onAnalysisCompleted.mockImplementation(() => resolve());
      });

      expect(controller.traces[0].status).toBe('analyzed');
      if (controller.traces[0].status === 'analyzed') {
        expect(controller.traces[0].format).toBe('Perfetto');
      }
    });

    it('should handle analysis errors', async () => {
      const file = createMockFile('error.pftrace');
      fakeAnalyzer.setError(file.name, new Error('Test analysis error'));

      controller.addFiles([file]);
      // Analysis starts immediately, so status should be 'analyzing'
      expect(controller.traces[0].status).toBe('analyzing');

      // Wait for async analysis to complete
      await new Promise<void>((resolve) => {
        onAnalysisCompleted.mockImplementation(() => resolve());
      });

      expect(controller.traces[0].status).toBe('error');
      if (controller.traces[0].status === 'error') {
        expect(controller.traces[0].error).toBe('Test analysis error');
      }
    });
  });
});
