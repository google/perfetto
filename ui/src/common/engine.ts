// Copyright (C) 2018 The Android Open Source Project
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

import {defer, Deferred} from '../base/deferred';
import {assertExists, assertTrue} from '../base/logging';
import {perfetto} from '../gen/protos';

import {ProtoRingBuffer} from './proto_ring_buffer';
import {
  ComputeMetricArgs,
  ComputeMetricResult,
  DisableAndReadMetatraceResult,
  QueryArgs,
  ResetTraceProcessorArgs,
} from './protos';
import {NUM, NUM_NULL, STR} from './query_result';
import {
  createQueryResult,
  QueryError,
  QueryResult,
  WritableQueryResult,
} from './query_result';
import {TimeSpan} from './time';

import TraceProcessorRpc = perfetto.protos.TraceProcessorRpc;
import TraceProcessorRpcStream = perfetto.protos.TraceProcessorRpcStream;
import TPM = perfetto.protos.TraceProcessorRpc.TraceProcessorMethod;

export interface LoadingTracker {
  beginLoading(): void;
  endLoading(): void;
}

export class NullLoadingTracker implements LoadingTracker {
  beginLoading(): void {}
  endLoading(): void {}
}


// This is used to skip the decoding of queryResult from protobufjs and deal
// with it ourselves. See the comment below around `QueryResult.decode = ...`.
interface QueryResultBypass {
  rawQueryResult: Uint8Array;
}

export interface TraceProcessorConfig {
  cropTrackEvents: boolean;
  ingestFtraceInRawTable: boolean;
  analyzeTraceProtoContent: boolean;
}

// Abstract interface of a trace proccessor.
// This is the TypeScript equivalent of src/trace_processor/rpc.h.
// There are two concrete implementations:
//   1. WasmEngineProxy: creates a Wasm module and interacts over postMessage().
//   2. HttpRpcEngine: connects to an external `trace_processor_shell --httpd`.
//      and interacts via fetch().
// In both cases, we have a byte-oriented pipe to interact with TraceProcessor.
// The derived class is only expected to deal with these two functions:
// 1. Implement the abstract rpcSendRequestBytes() function, sending the
//    proto-encoded TraceProcessorRpc requests to the TraceProcessor instance.
// 2. Call onRpcResponseBytes() when response data is received.
export abstract class Engine {
  abstract readonly id: string;
  private _cpus?: number[];
  private _numGpus?: number;
  private loadingTracker: LoadingTracker;
  private txSeqId = 0;
  private rxSeqId = 0;
  private rxBuf = new ProtoRingBuffer();
  private pendingParses = new Array<Deferred<void>>();
  private pendingEOFs = new Array<Deferred<void>>();
  private pendingResetTraceProcessors = new Array<Deferred<void>>();
  private pendingQueries = new Array<WritableQueryResult>();
  private pendingRestoreTables = new Array<Deferred<void>>();
  private pendingComputeMetrics = new Array<Deferred<ComputeMetricResult>>();
  private pendingReadMetatrace?: Deferred<DisableAndReadMetatraceResult>;
  private _isMetatracingEnabled = false;

  constructor(tracker?: LoadingTracker) {
    this.loadingTracker = tracker ? tracker : new NullLoadingTracker();
  }

  // Called to send data to the TraceProcessor instance. This turns into a
  // postMessage() or a HTTP request, depending on the Engine implementation.
  abstract rpcSendRequestBytes(data: Uint8Array): void;

  // Called when an inbound message is received by the Engine implementation
  // (e.g. onmessage for the Wasm case, on when HTTP replies are received for
  // the HTTP+RPC case).
  onRpcResponseBytes(dataWillBeRetained: Uint8Array) {
    // Note: when hitting the fastpath inside ProtoRingBuffer, the |data| buffer
    // is returned back by readMessage() (% subarray()-ing it) and held onto by
    // other classes (e.g., QueryResult). For both fetch() and Wasm we are fine
    // because every response creates a new buffer.
    this.rxBuf.append(dataWillBeRetained);
    for (;;) {
      const msg = this.rxBuf.readMessage();
      if (msg === undefined) break;
      this.onRpcResponseMessage(msg);
    }
  }

  // Parses a response message.
  // |rpcMsgEncoded| is a sub-array to to the start of a TraceProcessorRpc
  // proto-encoded message (without the proto preamble and varint size).
  private onRpcResponseMessage(rpcMsgEncoded: Uint8Array) {
    // Here we override the protobufjs-generated code to skip the parsing of the
    // new streaming QueryResult and instead passing it through like a buffer.
    // This is the overall problem: All trace processor responses are wrapped
    // into a perfetto.protos.TraceProcessorRpc proto message. In all cases %
    // TPM_QUERY_STREAMING, we want protobufjs to decode the proto bytes and
    // give us a structured object. In the case of TPM_QUERY_STREAMING, instead,
    // we want to deal with the proto parsing ourselves using the new
    // QueryResult.appendResultBatch() method, because that handled streaming
    // results more efficiently and skips several copies.
    // By overriding the decode method below, we achieve two things:
    // 1. We avoid protobufjs decoding the TraceProcessorRpc.query_result field.
    // 2. We stash (a view of) the original buffer into the |rawQueryResult| so
    //    the `case TPM_QUERY_STREAMING` below can take it.
    perfetto.protos.QueryResult.decode =
        (reader: protobuf.Reader, length: number) => {
          const res =
              perfetto.protos.QueryResult.create() as {} as QueryResultBypass;
          res.rawQueryResult =
              reader.buf.subarray(reader.pos, reader.pos + length);
          // All this works only if protobufjs returns the original ArrayBuffer
          // from |rpcMsgEncoded|. It should be always the case given the
          // current implementation. This check mainly guards against future
          // behavioral changes of protobufjs. We don't want to accidentally
          // hold onto some internal protobufjs buffer. We are fine holding
          // onto |rpcMsgEncoded| because those come from ProtoRingBuffer which
          // is buffer-retention-friendly.
          assertTrue(res.rawQueryResult.buffer === rpcMsgEncoded.buffer);
          reader.pos += length;
          return res as {} as perfetto.protos.QueryResult;
        };

    const rpc = TraceProcessorRpc.decode(rpcMsgEncoded);

    if (rpc.fatalError !== undefined && rpc.fatalError.length > 0) {
      throw new Error(`${rpc.fatalError}`);
    }

    // Allow restarting sequences from zero (when reloading the browser).
    if (rpc.seq !== this.rxSeqId + 1 && this.rxSeqId !== 0 && rpc.seq !== 0) {
      // "(ERR:rpc_seq)" is intercepted by error_dialog.ts to show a more
      // graceful and actionable error.
      throw new Error(`RPC sequence id mismatch cur=${rpc.seq} last=${
          this.rxSeqId} (ERR:rpc_seq)`);
    }

    this.rxSeqId = rpc.seq;

    let isFinalResponse = true;

    switch (rpc.response) {
      case TPM.TPM_APPEND_TRACE_DATA:
        const appendResult = assertExists(rpc.appendResult);
        const pendingPromise = assertExists(this.pendingParses.shift());
        if (appendResult.error && appendResult.error.length > 0) {
          pendingPromise.reject(appendResult.error);
        } else {
          pendingPromise.resolve();
        }
        break;
      case TPM.TPM_FINALIZE_TRACE_DATA:
        assertExists(this.pendingEOFs.shift()).resolve();
        break;
      case TPM.TPM_RESET_TRACE_PROCESSOR:
        assertExists(this.pendingResetTraceProcessors.shift()).resolve();
        break;
      case TPM.TPM_RESTORE_INITIAL_TABLES:
        assertExists(this.pendingRestoreTables.shift()).resolve();
        break;
      case TPM.TPM_QUERY_STREAMING:
        const qRes = assertExists(rpc.queryResult) as {} as QueryResultBypass;
        const pendingQuery = assertExists(this.pendingQueries[0]);
        pendingQuery.appendResultBatch(qRes.rawQueryResult);
        if (pendingQuery.isComplete()) {
          this.pendingQueries.shift();
        } else {
          isFinalResponse = false;
        }
        break;
      case TPM.TPM_COMPUTE_METRIC:
        const metricRes = assertExists(rpc.metricResult) as ComputeMetricResult;
        const pendingComputeMetric =
            assertExists(this.pendingComputeMetrics.shift());
        if (metricRes.error && metricRes.error.length > 0) {
          const error =
              new QueryError(`ComputeMetric() error: ${metricRes.error}`, {
                query: 'COMPUTE_METRIC',
              });
          pendingComputeMetric.reject(error);
        } else {
          pendingComputeMetric.resolve(metricRes);
        }
        break;
      case TPM.TPM_DISABLE_AND_READ_METATRACE:
        const metatraceRes =
            assertExists(rpc.metatrace) as DisableAndReadMetatraceResult;
        assertExists(this.pendingReadMetatrace).resolve(metatraceRes);
        this.pendingReadMetatrace = undefined;
        break;
      default:
        console.log(
            'Unexpected TraceProcessor response received: ', rpc.response);
        break;
    }  // switch(rpc.response);

    if (isFinalResponse) {
      this.loadingTracker.endLoading();
    }
  }

  // TraceProcessor methods below this point.
  // The methods below are called by the various controllers in the UI and
  // deal with marshalling / unmarshaling requests to/from TraceProcessor.


  // Push trace data into the engine. The engine is supposed to automatically
  // figure out the type of the trace (JSON vs Protobuf).
  parse(data: Uint8Array): Promise<void> {
    const asyncRes = defer<void>();
    this.pendingParses.push(asyncRes);
    const rpc = TraceProcessorRpc.create();
    rpc.request = TPM.TPM_APPEND_TRACE_DATA;
    rpc.appendTraceData = data;
    this.rpcSendRequest(rpc);
    return asyncRes;  // Linearize with the worker.
  }

  // Notify the engine that we reached the end of the trace.
  // Called after the last parse() call.
  notifyEof(): Promise<void> {
    const asyncRes = defer<void>();
    this.pendingEOFs.push(asyncRes);
    const rpc = TraceProcessorRpc.create();
    rpc.request = TPM.TPM_FINALIZE_TRACE_DATA;
    this.rpcSendRequest(rpc);
    return asyncRes;  // Linearize with the worker.
  }

  // Updates the TraceProcessor Config. This method creates a new
  // TraceProcessor instance, so it should be called before passing any trace
  // data.
  resetTraceProcessor(
      {cropTrackEvents, ingestFtraceInRawTable, analyzeTraceProtoContent}:
          TraceProcessorConfig): Promise<void> {
    const asyncRes = defer<void>();
    this.pendingResetTraceProcessors.push(asyncRes);
    const rpc = TraceProcessorRpc.create();
    rpc.request = TPM.TPM_RESET_TRACE_PROCESSOR;
    const args = rpc.resetTraceProcessorArgs = new ResetTraceProcessorArgs();
    args.dropTrackEventDataBefore = cropTrackEvents ?
        ResetTraceProcessorArgs.DropTrackEventDataBefore
            .TRACK_EVENT_RANGE_OF_INTEREST :
        ResetTraceProcessorArgs.DropTrackEventDataBefore.NO_DROP;
    args.ingestFtraceInRawTable = ingestFtraceInRawTable;
    args.analyzeTraceProtoContent = analyzeTraceProtoContent;
    this.rpcSendRequest(rpc);
    return asyncRes;
  }

  // Resets the trace processor state by destroying any table/views created by
  // the UI after loading.
  restoreInitialTables(): Promise<void> {
    const asyncRes = defer<void>();
    this.pendingRestoreTables.push(asyncRes);
    const rpc = TraceProcessorRpc.create();
    rpc.request = TPM.TPM_RESTORE_INITIAL_TABLES;
    this.rpcSendRequest(rpc);
    return asyncRes;  // Linearize with the worker.
  }

  // Shorthand for sending a compute metrics request to the engine.
  async computeMetric(metrics: string[]): Promise<ComputeMetricResult> {
    const asyncRes = defer<ComputeMetricResult>();
    this.pendingComputeMetrics.push(asyncRes);
    const rpc = TraceProcessorRpc.create();
    rpc.request = TPM.TPM_COMPUTE_METRIC;
    const args = rpc.computeMetricArgs = new ComputeMetricArgs();
    args.metricNames = metrics;
    args.format = ComputeMetricArgs.ResultFormat.TEXTPROTO;
    this.rpcSendRequest(rpc);
    return asyncRes;
  }

  // Issues a streaming query and retrieve results in batches.
  // The returned QueryResult object will be populated over time with batches
  // of rows (each batch conveys ~128KB of data and a variable number of rows).
  // The caller can decide whether to wait that all batches have been received
  // (by awaiting the returned object or calling result.waitAllRows()) or handle
  // the rows incrementally.
  //
  // Example usage:
  // const res = engine.query('SELECT foo, bar FROM table');
  // console.log(res.numRows());  // Will print 0 because we didn't await.
  // await(res.waitAllRows());
  // console.log(res.numRows());  // Will print the total number of rows.
  //
  // for (const it = res.iter({foo: NUM, bar:STR}); it.valid(); it.next()) {
  //   console.log(it.foo, it.bar);
  // }
  //
  // Optional |tag| (usually a component name) can be provided to allow
  // attributing trace processor workload to different UI components.
  query(sqlQuery: string, tag?: string): Promise<QueryResult>&QueryResult {
    const rpc = TraceProcessorRpc.create();
    rpc.request = TPM.TPM_QUERY_STREAMING;
    rpc.queryArgs = new QueryArgs();
    rpc.queryArgs.sqlQuery = sqlQuery;
    if (tag) {
      rpc.queryArgs.tag = tag;
    }
    const result = createQueryResult({
      query: sqlQuery,
    });
    this.pendingQueries.push(result);
    this.rpcSendRequest(rpc);
    return result;
  }

  isMetatracingEnabled(): boolean {
    return this._isMetatracingEnabled;
  }

  enableMetatrace(categories?: perfetto.protos.MetatraceCategories) {
    const rpc = TraceProcessorRpc.create();
    rpc.request = TPM.TPM_ENABLE_METATRACE;
    if (categories) {
      rpc.enableMetatraceArgs = new perfetto.protos.EnableMetatraceArgs();
      rpc.enableMetatraceArgs.categories = categories;
    }
    this._isMetatracingEnabled = true;
    this.rpcSendRequest(rpc);
  }

  stopAndGetMetatrace(): Promise<DisableAndReadMetatraceResult> {
    // If we are already finalising a metatrace, ignore the request.
    if (this.pendingReadMetatrace) {
      return Promise.reject(new Error('Already finalising a metatrace'));
    }

    const result = defer<DisableAndReadMetatraceResult>();

    const rpc = TraceProcessorRpc.create();
    rpc.request = TPM.TPM_DISABLE_AND_READ_METATRACE;
    this._isMetatracingEnabled = false;
    this.pendingReadMetatrace = result;
    this.rpcSendRequest(rpc);
    return result;
  }

  // Marshals the TraceProcessorRpc request arguments and sends the request
  // to the concrete Engine (Wasm or HTTP).
  private rpcSendRequest(rpc: TraceProcessorRpc) {
    rpc.seq = this.txSeqId++;
    // Each message is wrapped in a TraceProcessorRpcStream to add the varint
    // preamble with the size, which allows tokenization on the other end.
    const outerProto = TraceProcessorRpcStream.create();
    outerProto.msg.push(rpc);
    const buf = TraceProcessorRpcStream.encode(outerProto).finish();
    this.loadingTracker.beginLoading();
    this.rpcSendRequestBytes(buf);
  }

  // TODO(hjd): When streaming must invalidate this somehow.
  async getCpus(): Promise<number[]> {
    if (!this._cpus) {
      const cpus = [];
      const queryRes = await this.query(
          'select distinct(cpu) as cpu from sched order by cpu;');
      for (const it = queryRes.iter({cpu: NUM}); it.valid(); it.next()) {
        cpus.push(it.cpu);
      }
      this._cpus = cpus;
    }
    return this._cpus;
  }

  async getNumberOfGpus(): Promise<number> {
    if (!this._numGpus) {
      const result = await this.query(`
        select count(distinct(gpu_id)) as gpuCount
        from gpu_counter_track
        where name = 'gpufreq';
      `);
      this._numGpus = result.firstRow({gpuCount: NUM}).gpuCount;
    }
    return this._numGpus;
  }

  // TODO: This should live in code that's more specific to chrome, instead of
  // in engine.
  async getNumberOfProcesses(): Promise<number> {
    const result = await this.query('select count(*) as cnt from process;');
    return result.firstRow({cnt: NUM}).cnt;
  }

  async getTraceTimeBounds(): Promise<TimeSpan> {
    const result = await this.query(
        `select start_ts as startTs, end_ts as endTs from trace_bounds`);
    const bounds = result.firstRow({
      startTs: NUM,
      endTs: NUM,
    });
    return new TimeSpan(bounds.startTs / 1e9, bounds.endTs / 1e9);
  }

  async getTracingMetadataTimeBounds(): Promise<TimeSpan> {
    const queryRes = await this.query(`select
         name,
         int_value as intValue
         from metadata
         where name = 'tracing_started_ns' or name = 'tracing_disabled_ns'
         or name = 'all_data_source_started_ns'`);
    let startBound = -Infinity;
    let endBound = Infinity;
    const it = queryRes.iter({'name': STR, 'intValue': NUM_NULL});
    for (; it.valid(); it.next()) {
      const columnName = it.name;
      const timestamp = it.intValue;
      if (timestamp === null) continue;
      if (columnName === 'tracing_disabled_ns') {
        endBound = Math.min(endBound, timestamp / 1e9);
      } else {
        startBound = Math.max(startBound, timestamp / 1e9);
      }
    }

    return new TimeSpan(startBound, endBound);
  }

  getProxy(tag: string): EngineProxy {
    return new EngineProxy(this, tag);
  }
}

// Lightweight wrapper over Engine exposing only `query` method and annotating
// all queries going through it with a tag.
export class EngineProxy {
  private engine: Engine;
  private tag: string;

  constructor(engine: Engine, tag: string) {
    this.engine = engine;
    this.tag = tag;
  }

  query(sqlQuery: string, tag?: string): Promise<QueryResult>&QueryResult {
    return this.engine.query(sqlQuery, tag || this.tag);
  }
}
