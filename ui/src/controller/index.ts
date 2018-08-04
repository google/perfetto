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

import '../tracks/all_controller';

import {defer, Deferred} from '../base/deferred';
import {assertExists} from '../base/logging';
import {forwardRemoteCalls, Remote} from '../base/remote';
import {Action, addTrack} from '../common/actions';
import {rawQueryResultColumns, rawQueryResultIter, Row} from '../common/protos';
import {QueryResponse} from '../common/queries';
import {
  createEmptyState,
  EngineConfig,
  QueryConfig,
  State,
  TrackState
} from '../common/state';

import {Engine} from './engine';
import {rootReducer} from './reducer';
import {TrackController} from './track_controller';
import {trackControllerRegistry} from './track_controller_registry';
import {WasmEngineProxy} from './wasm_engine_proxy';

/**
 * |source| is either a URL where the Trace can be fetched from
 * or a File which contains the trace.
 */
async function fetchTrace(source: string|File): Promise<Blob> {
  if (source instanceof File) {
    return source;
  }
  const response = await fetch(source);
  return response.blob();
}

type EngineControllerState = 'init'|'waiting_for_file'|'loading'|'ready';
class EngineController {
  private readonly config: EngineConfig;
  private readonly controller: Controller;
  private readonly deferredOnReady: Set<Deferred<Engine>>;
  private _state: EngineControllerState;
  private blob?: Blob;
  private engine?: Engine;

  constructor(config: EngineConfig, controller: Controller) {
    this.controller = controller;
    this.config = config;
    this._state = 'init';
    this.deferredOnReady = new Set();
    this.transition('waiting_for_file');
  }

  get state(): EngineControllerState {
    return this._state;
  }

  private async transition(newState: EngineControllerState) {
    switch (newState) {
      case 'waiting_for_file':
        this.blob = await fetchTrace(this.config.source);
        this.transition('loading');
        break;
      case 'loading':
        const blob = assertExists<Blob>(this.blob);
        this.engine = await this.controller.createEngine(blob);
        this.transition('ready');
        break;
      case 'ready':
        const engine = assertExists<Engine>(this.engine);
        const numberOfCpus = await engine.getNumberOfCpus();
        const addToTrackActions = [];
        for (let i = 0; i < numberOfCpus; i++) {
          addToTrackActions.push(addTrack(this.config.id, 'CpuSliceTrack', i));
        }
        this.controller.dispatchMultiple(addToTrackActions);
        this.deferredOnReady.forEach(d => d.resolve(engine));
        this.deferredOnReady.clear();
        break;
      default:
        throw new Error(`No such state ${newState}`);
    }
    this._state = newState;
  }

  waitForReady(): Promise<Engine> {
    if (this.engine) return Promise.resolve(this.engine);
    const deferred = defer<Engine>();
    this.deferredOnReady.add(deferred);
    return deferred;
  }
}

class TrackControllerWrapper {
  private readonly config: TrackState;
  private readonly controller: Controller;
  private trackController?: TrackController;

  constructor(
      config: TrackState, controller: Controller,
      engineController: EngineController) {
    this.config = config;
    this.controller = controller;
    const publish = (data: {}) =>
        this.controller.publishTrackData(config.id, data);
    engineController.waitForReady().then(async engine => {
      const factory = trackControllerRegistry.get(this.config.kind);
      this.trackController = factory.create(config, engine, publish);
    });
  }

  onBoundsChange(start: number, end: number): void {
    if (!this.trackController) return;
    this.trackController.onBoundsChange(start, end);
  }
}

function firstN<T>(n: number, iter: IterableIterator<T>): T[] {
  const list = [];
  for (let i = 0; i < n; i++) {
    const {done, value} = iter.next();
    if (done) break;
    list.push(value);
  }
  return list;
}

class QueryController {
  constructor(
      config: QueryConfig, controller: Controller,
      engineController: EngineController) {
    engineController.waitForReady().then(async engine => {
      const start = performance.now();
      const rawResult = await engine.rawQuery({sqlQuery: config.query});
      const end = performance.now();
      const columns = rawQueryResultColumns(rawResult);
      const rows = firstN<Row>(100, rawQueryResultIter(rawResult));
      const result: QueryResponse = {
        id: config.id,
        query: config.query,
        durationMs: Math.round(end - start),
        totalRowCount: +rawResult.numRecords,
        columns,
        rows,
      };
      controller.publishTrackData(config.id, result);
    });
  }
}

class Controller {
  private state: State;
  private _frontend?: FrontendProxy;
  private readonly engines: Map<string, EngineController>;
  private readonly tracks: Map<string, TrackControllerWrapper>;
  private readonly queries: Map<string, QueryController>;

  constructor() {
    this.state = createEmptyState();
    this.engines = new Map();
    this.tracks = new Map();
    this.queries = new Map();
  }

  get frontend(): FrontendProxy {
    if (!this._frontend) throw new Error('No FrontendProxy');
    return this._frontend;
  }

  initAndGetState(frontendProxyPort: MessagePort): State {
    this._frontend = new FrontendProxy(new Remote(frontendProxyPort));
    return this.state;
  }

  dispatch(action: Action): void {
    this.dispatchMultiple([action]);
  }

  dispatchMultiple(actions: Action[]): void {
    for (const action of actions) {
      this.state = rootReducer(this.state, action);
    }

    // TODO(hjd): Handle teardown.
    for (const config of Object.values<EngineConfig>(this.state.engines)) {
      if (this.engines.has(config.id)) continue;
      this.engines.set(config.id, new EngineController(config, this));
    }

    // TODO(hjd): Handle teardown.
    for (const config of Object.values<TrackState>(this.state.tracks)) {
      if (this.tracks.has(config.id)) continue;
      const engine = this.engines.get(config.engineId)!;
      this.tracks.set(
          config.id, new TrackControllerWrapper(config, this, engine));
    }

    // TODO(hjd): Handle teardown.
    for (const config of Object.values<QueryConfig>(this.state.queries)) {
      if (this.queries.has(config.id)) continue;
      const engine = this.engines.get(config.engineId)!;
      this.queries.set(config.id, new QueryController(config, this, engine));
    }

    this.frontend.updateState(this.state);
  }

  publishTrackData(id: string, data: {}) {
    this.frontend.publishTrackData(id, data);
  }

  async createEngine(blob: Blob): Promise<Engine> {
    const port = await this.frontend.createWasmEnginePort();
    return WasmEngineProxy.create(port, blob);
  }
}

/**
 * Proxy for talking to the main thread.
 * TODO(hjd): Reduce the boilerplate.
 */
class FrontendProxy {
  private readonly remote: Remote;

  constructor(remote: Remote) {
    this.remote = remote;
  }

  updateState(state: State) {
    return this.remote.send<void>('updateState', [state]);
  }

  createWasmEnginePort() {
    return this.remote.send<MessagePort>('createWasmEnginePort', []);
  }

  publishTrackData(id: string, data: {}) {
    return this.remote.send<void>('publishTrackData', [id, data]);
  }
}

function main() {
  const controller = new Controller();
  forwardRemoteCalls(self as {} as MessagePort, controller);
}

main();
