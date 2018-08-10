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
import {Remote} from '../base/remote';
import {
  Action,
  addChromeSliceTrack,
  addTrack,
  deleteQuery,
  navigate,
  setEngineReady
} from '../common/actions';
import {PERMALINK_ID, saveState, saveTrace} from '../common/permalinks';
import {rawQueryResultColumns, rawQueryResultIter, Row} from '../common/protos';
import {QueryResponse} from '../common/queries';
import {
  createEmptyState,
  EngineConfig,
  PermalinkConfig,
  QueryConfig,
  State,
  TrackState,
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
        const addToTrackActions: Action[] = [];
        if (numberOfCpus > 0) {
          // This is a sched slice trace.
          for (let i = 0; i < numberOfCpus; i++) {
            addToTrackActions.push(
                addTrack(this.config.id, 'CpuSliceTrack', i));
          }
        } else if ((await engine.getNumberOfProcesses()) > 0) {
          const threadQuery = await engine.rawQuery({
            sqlQuery:
                'select upid, utid, tid, thread.name, max(slices.depth) ' +
                'from thread inner join slices using(utid) group by utid'
          });
          for (let i = 0; i < threadQuery.numRecords; i++) {
            const upid = threadQuery.columns[0].longValues![i];
            const utid = threadQuery.columns[1].longValues![i];
            const threadId = threadQuery.columns[2].longValues![i];
            let threadName = threadQuery.columns[3].stringValues![i];
            threadName += `[${threadId}]`;
            const maxDepth = threadQuery.columns[4].longValues![i];
            addToTrackActions.push(addChromeSliceTrack(
                this.config.id,
                'ChromeSliceTrack',
                upid as number,
                utid as number,
                threadName,
                maxDepth as number));
          }
        }
        this.controller.dispatchMultiple(addToTrackActions);
        this.deferredOnReady.forEach(d => d.resolve(engine));
        this.deferredOnReady.clear();
        this.controller.dispatch(setEngineReady(this.config.id));
        this.controller.dispatch(navigate('/viewer'));
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
      const rows = firstN<Row>(10000, rawQueryResultIter(rawResult));
      const result: QueryResponse = {
        id: config.id,
        query: config.query,
        durationMs: Math.round(end - start),
        error: rawResult.error,
        totalRowCount: +rawResult.numRecords,
        columns,
        rows,
      };
      controller.publishQueryResult(config.id, result);
      controller.dispatch(deleteQuery(config.id));
    });
  }
}

async function createPermalink(
    config: PermalinkConfig, controller: Controller) {
  const state = {...config.state};
  state.engines = {...state.engines};
  state.permalink = null;
  for (const engine of Object.values<EngineConfig>(state.engines)) {
    if (typeof engine.source === 'string') {
      continue;
    }
    const url = await saveTrace(engine.source);
    // TODO(hjd): Post to controller.
    engine.source = url;
  }
  const url = await saveState(state);
  controller.publishTrackData(PERMALINK_ID, {
    url,
  });
}

class PermalinkController {
  private readonly controller: Controller;
  private config: PermalinkConfig|null;

  constructor(controller: Controller) {
    this.controller = controller;
    this.config = null;
  }

  updateConfig(config: PermalinkConfig|null) {
    if (this.config === config) {
      return;
    }
    this.config = config;
    if (this.config) {
      createPermalink(this.config, this.controller);
    }
  }
}

class Controller {
  private state: State;
  private readonly frontend: FrontendProxy;
  private readonly engines: Map<string, EngineController>;
  private readonly tracks: Map<string, TrackControllerWrapper>;
  private readonly queries: Map<string, QueryController>;
  private readonly permalink: PermalinkController;

  constructor(frontend: FrontendProxy) {
    this.state = createEmptyState();
    this.frontend = frontend;
    this.engines = new Map();
    this.tracks = new Map();
    this.queries = new Map();
    this.permalink = new PermalinkController(this);
  }

  dispatch(action: Action): void {
    this.dispatchMultiple([action]);
  }

  dispatchMultiple(actions: Action[]): void {
    for (const action of actions) {
      this.state = rootReducer(this.state, action);
    }

    this.permalink.updateConfig(this.state.permalink);

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

    // Delete queries that aren't in the state anymore.
    for (const id of this.queries.keys()) {
      if (this.state.queries[id] === undefined) {
        this.queries.delete(id);
      }
    }
    for (const config of Object.values<QueryConfig>(this.state.queries)) {
      if (this.queries.has(config.id)) continue;
      const engine = this.engines.get(config.engineId)!;
      if (engine === undefined) continue;
      this.queries.set(config.id, new QueryController(config, this, engine));
    }

    this.frontend.updateState(this.state);
  }

  publishTrackData(id: string, data: {}) {
    this.frontend.publishTrackData(id, data);
  }


  publishQueryResult(id: string, data: {}) {
    this.frontend.publishQueryResult(id, data);
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

  publishQueryResult(id: string, data: {}) {
    return this.remote.send<void>('publishQueryResult', [id, data]);
  }
}

function main() {
  const port = self as {} as MessagePort;
  port.onmessage = ({data}) => {
    const frontendPort = data as MessagePort;
    const frontend = new FrontendProxy(new Remote(frontendPort));
    const controller = new Controller(frontend);
    port.onmessage = ({data}) => controller.dispatch(data);
  };
}

main();
