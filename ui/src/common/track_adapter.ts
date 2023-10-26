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

import m from 'mithril';
import {v4 as uuidv4} from 'uuid';

import {assertExists} from '../base/logging';
import {duration, Span, time} from '../base/time';
import {EngineProxy} from '../common/engine';
import {PxSpan, TimeScale} from '../frontend/time_scale';
import {NewTrackArgs, SliceRect} from '../frontend/track';

import {BasicAsyncTrack} from './basic_async_track';

export {EngineProxy} from '../common/engine';
export {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../common/query_result';
export {Store} from '../frontend/store';

// This is an adapter to convert old style controller based tracks to new style
// tracks.
export class TrackWithControllerAdapter<Config, Data> extends
    BasicAsyncTrack<Data> {
  private track: TrackAdapter<Config, Data>;
  private controller: TrackControllerAdapter<Config, Data>;
  private isSetup = false;

  constructor(
      engine: EngineProxy, trackKey: string, config: Config,
      Track: TrackAdapterClass<Config, Data>,
      Controller: TrackControllerAdapterClass<Config, Data>) {
    super();
    const args: NewTrackArgs = {
      trackKey,
      engine,
    };
    this.track = new Track(args);
    this.track.setConfig(config);
    this.track.setDataSource(() => this.data);
    this.controller = new Controller(config, engine);
  }

  onDestroy(): void {
    this.track.onDestroy();
    this.controller.onDestroy();
    super.onDestroy();
  }

  getSliceRect(
      visibleTimeScale: TimeScale, visibleWindow: Span<time, bigint>,
      windowSpan: PxSpan, tStart: time, tEnd: time, depth: number): SliceRect
      |undefined {
    return this.track.getSliceRect(
        visibleTimeScale, visibleWindow, windowSpan, tStart, tEnd, depth);
  }

  getHeight(): number {
    return this.track.getHeight();
  }

  getTrackShellButtons(): m.Children {
    return this.track.getTrackShellButtons();
  }

  onMouseMove(position: {x: number; y: number;}): void {
    this.track.onMouseMove(position);
  }

  onMouseClick(position: {x: number; y: number;}): boolean {
    return this.track.onMouseClick(position);
  }

  onMouseOut(): void {
    this.track.onMouseOut();
  }

  onFullRedraw(): void {
    this.track.onFullRedraw();
  }

  async onBoundsChange(start: time, end: time, resolution: duration):
      Promise<Data> {
    if (!this.isSetup) {
      await this.controller.onSetup();
      this.isSetup = true;
    }
    return await this.controller.onBoundsChange(start, end, resolution);
  }

  renderCanvas(ctx: CanvasRenderingContext2D): void {
    this.track.renderCanvas(ctx);
  }
}

// Extend from this class instead of `Track` to use existing track
// implementations with `TrackWithControllerAdapter`.
export abstract class TrackAdapter<Config, Data> {
  private _config?: Config;
  private dataSource?: () => Data | undefined;
  protected trackKey: string;

  get config(): Config {
    return assertExists(this._config);
  }

  setConfig(config: Config) {
    this._config = config;
  }

  data(): Data|undefined {
    return this.dataSource && this.dataSource();
  }

  // A callback used to fetch the latest data
  setDataSource(dataSource: () => Data | undefined) {
    this.dataSource = dataSource;
  }

  constructor(args: NewTrackArgs) {
    this.trackKey = args.trackKey;
  }

  abstract renderCanvas(ctx: CanvasRenderingContext2D): void;

  getSliceRect(
      _visibleTimeScale: TimeScale, _visibleWindow: Span<time, bigint>,
      _windowSpan: PxSpan, _tStart: time, _tEnd: time,
      _depth: number): SliceRect|undefined {
    return undefined;
  }

  getHeight(): number {
    return 40;
  }

  getTrackShellButtons(): m.Children {
    return [];
  }

  onMouseMove(_position: {x: number, y: number}) {}

  // Returns whether the mouse click has selected something.
  // Used to prevent further propagation if necessary.
  onMouseClick(_position: {x: number, y: number}): boolean {
    return false;
  }

  onMouseOut(): void {}

  onFullRedraw(): void {}

  onDestroy(): void {}
}

type TrackAdapterClass<Config, Data> = {
  new (args: NewTrackArgs): TrackAdapter<Config, Data>
}

function hasNamespace(config: unknown): config is {
  namespace: string
} {
  return !!config && typeof config === 'object' && 'namespace' in config;
}

// Extend from this class instead of `TrackController` to use existing track
// controller implementations with `TrackWithControllerAdapter`.
export abstract class TrackControllerAdapter<Config, Data> {
  // This unique ID is just used to create the table names.
  // In the future we should probably use the track instance ID, but for now we
  // don't have access to it.
  private uuid = uuidv4();

  constructor(protected config: Config, protected engine: EngineProxy) {}

  protected async query(query: string) {
    const result = await this.engine.query(query);
    return result;
  }

  abstract onBoundsChange(start: time, end: time, resolution: duration):
      Promise<Data>;

  async onSetup(): Promise<void> {}
  async onDestroy(): Promise<void> {}

  // Returns a valid SQL table name with the given prefix that should be unique
  // for each track.
  tableName(prefix: string) {
    // Derive table name from, since that is unique for each track.
    // Track ID can be UUID but '-' is not valid for sql table name.
    const idSuffix = this.uuid.split('-').join('_');
    return `${prefix}_${idSuffix}`;
  }

  namespaceTable(tableName: string): string {
    if (hasNamespace(this.config)) {
      return this.config.namespace + '_' + tableName;
    } else {
      return tableName;
    }
  }
}

type TrackControllerAdapterClass<Config, Data> = {
  new (config: Config, engine: EngineProxy):
      TrackControllerAdapter<Config, Data>
}
