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
import {TraceFileAnalyzed} from './multi_trace_types';
import {TraceAnalysisResult, TraceAnalyzer} from './trace_analyzer';

// Helper to create a mock TraceFileAnalyzed object for manual mode tests
function createMockTrace(
  uuid: string,
  clocks: string[],
  syncMode: 'AUTOMATIC' | 'MANUAL' = 'AUTOMATIC',
): TraceFileAnalyzed {
  return {
    uuid,
    file: new File([], `${uuid}.pftrace`),
    status: 'analyzed',
    format: 'Perfetto',
    clocks: clocks.map((name) => ({name, count: 100})),
    syncMode,
    syncConfig: {syncMode: 'REFERENCE', referenceClock: ''}, // Default config
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
    _onProgress: (progress: number) => void,
  ): Promise<TraceAnalysisResult> {
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

  beforeEach(() => {
    fakeAnalyzer = new FakeTraceAnalyzer();
    onStateChanged = jest.fn();
    controller = new MultiTraceController(fakeAnalyzer, onStateChanged);
  });

  it('should initialize with no traces or errors', () => {
    expect(controller.traces).toHaveLength(0);
    expect(controller.syncError).toBeUndefined();
  });

  it('should set a single trace as a root', async () => {
    const file = createMockFile('trace1.pftrace');
    fakeAnalyzer.setResult(file.name, {
      format: 'Perfetto',
      clocks: [{name: 'BOOTTIME', count: 1}],
    });

    await controller.addFiles([file]);

    const trace = controller.traces[0] as TraceFileAnalyzed;
    expect(trace.syncConfig.syncMode).toEqual('REFERENCE');
  });

  it('should sync two traces based on preferred clock order', async () => {
    const file1 = createMockFile('trace1.pftrace');
    const file2 = createMockFile('trace2.pftrace');
    // trace1 has a lower priority clock
    fakeAnalyzer.setResult(file1.name, {
      format: 'Perfetto',
      clocks: [{name: 'MONOTONIC', count: 1}],
    });
    // trace2 has a higher priority clock
    fakeAnalyzer.setResult(file2.name, {
      format: 'Perfetto',
      clocks: [{name: 'BOOTTIME', count: 1}],
    });

    await controller.addFiles([file1, file2]);

    const trace1 = controller.traces.find(
      (t) => t.file.name === file1.name,
    ) as TraceFileAnalyzed;
    const trace2 = controller.traces.find(
      (t) => t.file.name === file2.name,
    ) as TraceFileAnalyzed;

    // Trace 2 should be root as it has the higher priority clock
    expect(trace2.syncConfig.syncMode).toEqual('REFERENCE');
    // In this specific test, they can't sync, so trace1 will also be a root.
    expect(trace1.syncConfig.syncMode).toEqual('REFERENCE');
  });

  it('should select the highest priority clock for a single root trace', async () => {
    const file = createMockFile('trace1.pftrace');
    const clocks = [
      {name: 'REALTIME_COARSE', count: 1},
      {name: 'REALTIME', count: 1},
      {name: 'MONOTONIC_RAW', count: 1},
      {name: 'MONOTONIC_COARSE', count: 1},
      {name: 'MONOTONIC', count: 1},
      {name: 'BOOTTIME', count: 1},
    ];
    fakeAnalyzer.setResult(file.name, {format: 'Perfetto', clocks});

    await controller.addFiles([file]);

    const trace = controller.traces[0] as TraceFileAnalyzed;
    expect(trace.syncConfig.syncMode).toEqual('REFERENCE');
    if (trace.syncConfig.syncMode === 'REFERENCE') {
      expect(trace.syncConfig.referenceClock).toEqual('BOOTTIME');
    }
  });

  it('should choose the highest priority common clock for sync', async () => {
    const file1 = createMockFile('trace1.pftrace');
    const file2 = createMockFile('trace2.pftrace');
    // Both traces share a high and low priority clock
    const clocks = [
      {name: 'REALTIME_COARSE', count: 1},
      {name: 'BOOTTIME', count: 1},
    ];
    fakeAnalyzer.setResult(file1.name, {format: 'Perfetto', clocks});
    fakeAnalyzer.setResult(file2.name, {format: 'Perfetto', clocks});

    await controller.addFiles([file1, file2]);

    const trace1 = controller.traces[0] as TraceFileAnalyzed;
    const trace2 = controller.traces[1] as TraceFileAnalyzed;

    // Assuming trace1 becomes the root
    const syncTrace =
      trace1.syncConfig.syncMode === 'REFERENCE' ? trace2 : trace1;
    const rootTrace =
      trace1.syncConfig.syncMode === 'REFERENCE' ? trace1 : trace2;

    expect(syncTrace.syncConfig.syncMode).toEqual('SYNC_TO_OTHER');
    if (syncTrace.syncConfig.syncMode === 'SYNC_TO_OTHER') {
      expect(syncTrace.syncConfig.syncClock?.anchorTraceUuid).toEqual(
        rootTrace.uuid,
      );
      // This is the key check: it must use the best clock.
      expect(syncTrace.syncConfig.syncClock?.thisTraceClock).toEqual(
        'BOOTTIME',
      );
      expect(syncTrace.syncConfig.syncClock?.anchorClock).toEqual('BOOTTIME');
    } else {
      fail('One trace should be syncing to the other');
    }
  });

  it('should handle multiple disconnected traces', async () => {
    const file1 = createMockFile('trace1.pftrace');
    const file2 = createMockFile('trace2.pftrace');
    fakeAnalyzer.setResult(file1.name, {
      format: 'Perfetto',
      clocks: [{name: 'BOOTTIME', count: 1}],
    });
    fakeAnalyzer.setResult(file2.name, {
      format: 'Perfetto',
      clocks: [{name: 'MONOTONIC', count: 1}],
    });

    await controller.addFiles([file1, file2]);

    const trace1 = controller.traces[0] as TraceFileAnalyzed;
    const trace2 = controller.traces[1] as TraceFileAnalyzed;

    // Both should become roots as they can't be synced
    expect(trace1.syncConfig.syncMode).toEqual('REFERENCE');
    expect(trace2.syncConfig.syncMode).toEqual('REFERENCE');
  });

  it('should respect a manual root', () => {
    const trace1 = createMockTrace('uuid1', ['BOOTTIME']);
    const trace2 = createMockTrace('uuid2', ['BOOTTIME'], 'MANUAL');
    trace2.syncConfig = {syncMode: 'REFERENCE', referenceClock: 'BOOTTIME'};
    controller.setTracesForTesting([trace1, trace2]);

    controller.recomputeSync();

    // trace2 is the manual root, so trace1 must sync to it
    expect(trace1.syncConfig.syncMode).toEqual('SYNC_TO_OTHER');
    if (trace1.syncConfig.syncMode === 'SYNC_TO_OTHER') {
      expect(trace1.syncConfig.syncClock.anchorTraceUuid).toEqual('uuid2');
    }
  });

  it('should detect and report multiple manual roots', () => {
    const trace1 = createMockTrace('uuid1', ['BOOTTIME'], 'MANUAL');
    trace1.syncConfig = {syncMode: 'REFERENCE', referenceClock: 'BOOTTIME'};
    const trace2 = createMockTrace('uuid2', ['MONOTONIC'], 'MANUAL');
    trace2.syncConfig = {syncMode: 'REFERENCE', referenceClock: 'MONOTONIC'};
    controller.setTracesForTesting([trace1, trace2]);

    controller.recomputeSync();

    expect(controller.syncError).toEqual(
      'Only one reference clock can be chosen.',
    );
  });

  it('should respect a manual sync configuration', () => {
    const trace1 = createMockTrace('uuid1', ['BOOTTIME']);
    const trace2 = createMockTrace('uuid2', ['MONOTONIC'], 'MANUAL');
    const trace3 = createMockTrace('uuid3', ['MONOTONIC']);

    trace2.syncConfig = {
      syncMode: 'SYNC_TO_OTHER',
      syncClock: {
        thisTraceClock: 'MONOTONIC',
        anchorTraceUuid: 'uuid1',
        anchorClock: 'BOOTTIME',
        offset: {kind: 'valid', raw: '0', value: 0},
      },
    };
    controller.setTracesForTesting([trace1, trace2, trace3]);

    controller.recomputeSync();

    expect(trace1.syncConfig.syncMode).toEqual('REFERENCE');
    if (trace2.syncConfig.syncMode === 'SYNC_TO_OTHER') {
      expect(trace2.syncConfig.syncClock.anchorTraceUuid).toEqual('uuid1');
    }
    if (trace3.syncConfig.syncMode === 'SYNC_TO_OTHER') {
      expect(trace3.syncConfig.syncClock.anchorTraceUuid).toEqual('uuid2');
      expect(trace3.syncConfig.syncClock.thisTraceClock).toEqual('MONOTONIC');
    }
  });

  describe('getLoadingError', () => {
    it('returns NO_TRACES when no traces are loaded', () => {
      expect(controller.getLoadingError()).toEqual('NO_TRACES');
    });

    it('returns TRACE_ERROR when a trace has an error', async () => {
      const file = createMockFile('error_trace.pftrace');
      fakeAnalyzer.setError(file.name, new Error('Analysis failed'));
      await controller.addFiles([file]);
      expect(controller.getLoadingError()).toEqual('TRACE_ERROR');
    });

    it('returns SYNC_ERROR when multiple manual roots are set', () => {
      const trace1 = createMockTrace('uuid1', ['BOOTTIME'], 'MANUAL');
      trace1.syncConfig = {syncMode: 'REFERENCE', referenceClock: 'BOOTTIME'};
      const trace2 = createMockTrace('uuid2', ['MONOTONIC'], 'MANUAL');
      trace2.syncConfig = {syncMode: 'REFERENCE', referenceClock: 'MONOTONIC'};
      controller.setTracesForTesting([trace1, trace2]);
      controller.recomputeSync();
      expect(controller.getLoadingError()).toEqual('SYNC_ERROR');
    });

    it('returns INCOMPLETE_CONFIG when a root clock is not set', () => {
      const trace = createMockTrace('uuid1', ['BOOTTIME'], 'MANUAL');
      trace.syncConfig = {syncMode: 'REFERENCE', referenceClock: undefined};
      controller.setTracesForTesting([trace]);
      expect(controller.getLoadingError()).toEqual('INCOMPLETE_CONFIG');
    });

    it('returns INCOMPLETE_CONFIG when a sync target is not fully set', () => {
      const trace = createMockTrace('uuid1', ['BOOTTIME'], 'MANUAL');
      trace.syncConfig = {
        syncMode: 'SYNC_TO_OTHER',
        syncClock: {
          thisTraceClock: 'BOOTTIME',
          anchorClock: undefined,
          offset: {kind: 'valid', raw: '0', value: 0},
        },
      };
      controller.setTracesForTesting([trace]);
      expect(controller.getLoadingError()).toEqual('INCOMPLETE_CONFIG');
    });

    it('returns undefined when configuration is valid', () => {
      const trace = createMockTrace('uuid1', ['BOOTTIME'], 'MANUAL');
      trace.syncConfig = {syncMode: 'REFERENCE', referenceClock: 'BOOTTIME'};
      controller.setTracesForTesting([trace]);
      expect(controller.getLoadingError()).toBeUndefined();
    });
  });

  describe('addFiles', () => {
    it('should add a file that fails analysis', async () => {
      const file = createMockFile('error.pftrace');
      fakeAnalyzer.setError(file.name, new Error('Test analysis error'));
      await controller.addFiles([file]);
      expect(controller.traces.length).toBe(1);
      const trace = controller.traces[0];
      expect(trace.status).toBe('error');
      if (trace.status === 'error') {
        expect(trace.error).toBe('Test analysis error');
      }
    });

    it('should add files to a non-empty list', async () => {
      const file1 = createMockFile('trace1.pftrace');
      fakeAnalyzer.setResult(file1.name, {
        format: 'Perfetto',
        clocks: [{name: 'BOOTTIME', count: 1}],
      });
      await controller.addFiles([file1]);
      expect(controller.traces.length).toBe(1);

      const file2 = createMockFile('trace2.pftrace');
      fakeAnalyzer.setResult(file2.name, {
        format: 'Perfetto',
        clocks: [{name: 'BOOTTIME', count: 1}],
      });
      await controller.addFiles([file2]);
      expect(controller.traces.length).toBe(2);
    });
  });

  describe('removeTrace', () => {
    it('should correctly recompute sync when an anchor trace is removed', () => {
      const trace1 = createMockTrace('uuid1', ['BOOTTIME']);
      const trace2 = createMockTrace('uuid2', ['BOOTTIME']);
      controller.setTracesForTesting([trace1, trace2]);
      controller.recomputeSync();

      // Ensure trace2 is synced to trace1
      const syncedTrace = controller.traces.find(
        (t) => (t as TraceFileAnalyzed).syncConfig.syncMode === 'SYNC_TO_OTHER',
      ) as TraceFileAnalyzed;
      const referenceTrace = controller.traces.find(
        (t) => (t as TraceFileAnalyzed).syncConfig.syncMode === 'REFERENCE',
      ) as TraceFileAnalyzed;
      expect(syncedTrace).toBeDefined();
      expect(referenceTrace).toBeDefined();

      // Remove the reference trace
      controller.removeTrace(referenceTrace.uuid);

      // The synced trace should now become a reference trace
      expect(syncedTrace.syncConfig.syncMode).toBe('REFERENCE');
    });
  });

  describe('findBestClock', () => {
    it('should return the first clock if no preferred clock is found', () => {
      const trace = createMockTrace('uuid1', [
        'UNCOMMON_CLOCK',
        'ANOTHER_CLOCK',
      ]);
      expect(controller.findBestClock(trace)).toBe('UNCOMMON_CLOCK');
    });

    it('should return undefined if there are no clocks', () => {
      const trace = createMockTrace('uuid1', []);
      expect(controller.findBestClock(trace)).toBeUndefined();
    });
  });

  describe('setTraceOffset', () => {
    let trace: TraceFileAnalyzed;

    beforeEach(() => {
      trace = createMockTrace('uuid1', [], 'MANUAL');
    });

    it('should parse valid integer strings', () => {
      trace.syncConfig = {
        syncMode: 'SYNC_TO_OTHER',
        syncClock: {offset: {kind: 'valid', raw: '0', value: 0}},
      };
      controller.setTracesForTesting([trace]);
      if (trace.syncConfig.syncMode === 'SYNC_TO_OTHER') {
        const anchorLink = trace.syncConfig.syncClock;
        controller.setTraceOffset(anchorLink, '123');
        expect(anchorLink.offset).toEqual({
          kind: 'valid',
          raw: '123',
          value: 123,
        });

        controller.setTraceOffset(anchorLink, '-456');
        expect(anchorLink.offset).toEqual({
          kind: 'valid',
          raw: '-456',
          value: -456,
        });

        controller.setTraceOffset(anchorLink, '0');
        expect(anchorLink.offset).toEqual({kind: 'valid', raw: '0', value: 0});
      }
    });

    it('should invalidate non-integer strings', () => {
      trace.syncConfig = {
        syncMode: 'SYNC_TO_OTHER',
        syncClock: {offset: {kind: 'valid', raw: '0', value: 0}},
      };
      controller.setTracesForTesting([trace]);
      if (trace.syncConfig.syncMode === 'SYNC_TO_OTHER') {
        const anchorLink = trace.syncConfig.syncClock;
        controller.setTraceOffset(anchorLink, '123.45');
        expect(anchorLink.offset.kind).toBe('invalid');

        controller.setTraceOffset(anchorLink, 'abc');
        expect(anchorLink.offset.kind).toBe('invalid');

        controller.setTraceOffset(anchorLink, '');
        expect(anchorLink.offset.kind).toBe('invalid');

        controller.setTraceOffset(anchorLink, '-');
        expect(anchorLink.offset.kind).toBe('invalid');
      }
    });

    it('should call onStateChanged when offset is modified', () => {
      trace.syncConfig = {
        syncMode: 'SYNC_TO_OTHER',
        syncClock: {offset: {kind: 'valid', raw: '0', value: 0}},
      };
      controller.setTracesForTesting([trace]);
      if (trace.syncConfig.syncMode === 'SYNC_TO_OTHER') {
        const anchorLink = trace.syncConfig.syncClock;
        controller.setTraceOffset(anchorLink, '123');
        expect(onStateChanged).toHaveBeenCalled();
      }
    });
  });

  describe('selectTrace', () => {
    it('should select a trace by UUID', () => {
      const trace1 = createMockTrace('uuid1', []);
      const trace2 = createMockTrace('uuid2', []);
      controller.setTracesForTesting([trace1, trace2]);

      controller.selectTrace('uuid2');
      expect(controller.selectedTrace?.uuid).toBe('uuid2');
    });

    it('should deselect when a non-existent UUID is provided', () => {
      const trace1 = createMockTrace('uuid1', []);
      controller.setTracesForTesting([trace1]);
      controller.selectTrace('uuid1');
      expect(controller.selectedTrace?.uuid).toBe('uuid1');

      controller.selectTrace('non-existent-uuid');
      expect(controller.selectedTrace).toBeUndefined();
    });
  });
});
