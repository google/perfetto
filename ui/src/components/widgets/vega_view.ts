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
import * as vega from 'vega';
import * as vegaLite from 'vega-lite';
import {getErrorMessage} from '../../base/errors';
import {isString, shallowEquals} from '../../base/object_utils';
import {SimpleResizeObserver} from '../../base/resize_observer';
import {Engine} from '../../trace_processor/engine';
import {QueryError} from '../../trace_processor/query_result';
import {Spinner} from '../../widgets/spinner';

function isVegaLite(spec: unknown): boolean {
  if (typeof spec === 'object') {
    const schema = (spec as {$schema: unknown})['$schema'];
    if (schema !== undefined && isString(schema)) {
      // If the schema is available use that:
      return schema.includes('vega-lite');
    }
  }
  // Otherwise assume vega-lite:
  return true;
}

// Vega-Lite specific interactions
// types (https://vega.github.io/vega-lite/docs/selection.html#select)
export enum VegaLiteSelectionTypes {
  INTERVAL = 'interval',
  POINT = 'point',
}

// Vega-Lite Field Types
// These are for axis field (data) types
// https://vega.github.io/vega-lite/docs/type.html
export type VegaLiteFieldType =
  | 'quantitative'
  | 'temporal'
  | 'ordinal'
  | 'nominal'
  | 'geojson';

// Vega-Lite supported aggregation operations
// https://vega.github.io/vega-lite/docs/aggregate.html#ops
export type VegaLiteAggregationOps =
  | 'count'
  | 'valid'
  | 'values'
  | 'missing'
  | 'distinct'
  | 'sum'
  | 'product'
  | 'mean'
  | 'average'
  | 'variance'
  | 'variancep'
  | 'stdev'
  | 'stdevp'
  | 'stderr'
  | 'median'
  | 'q1'
  | 'q3'
  | 'ci0'
  | 'ci1'
  | 'min'
  | 'max'
  | 'argmin'
  | 'argmax';

export type VegaEventType =
  | 'click'
  | 'dblclick'
  | 'dragenter'
  | 'dragleave'
  | 'dragover'
  | 'keydown'
  | 'keypress'
  | 'keyup'
  | 'mousedown'
  | 'mousemove'
  | 'mouseout'
  | 'mouseover'
  | 'mouseup'
  | 'mousewheel'
  | 'touchend'
  | 'touchmove'
  | 'touchstart'
  | 'wheel';

export interface VegaViewData {
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  [name: string]: any;
}

type VegaSignalListenerHandler = (args: {
  view: vega.View;
  name: string;
  value: vega.SignalValue;
}) => void;
type VegaEventListenerHandler = (args: {
  view: vega.View;
  event: vega.ScenegraphEvent;
  item?: vega.Item | null;
}) => void;

interface VegaViewAttrs {
  spec: string;
  data: VegaViewData;
  engine?: Engine;
  signalHandlers?: {
    readonly name: string;
    readonly handler: VegaSignalListenerHandler;
  }[];
  eventHandlers?: {
    readonly name: string;
    readonly handler: VegaEventListenerHandler;
  }[];
  onViewDestroyed?: () => void;
}

// VegaWrapper is in exactly one of these states:
enum Status {
  // Has not visualisation to render.
  Empty,
  // Currently loading the visualisation.
  Loading,
  // Failed to load or render the visualisation. The reason is
  // retrievable via |error|.
  Error,
  // Displaying a visualisation:
  Done,
}

class EngineLoader implements vega.Loader {
  private engine?: Engine;
  private loader: vega.Loader;

  constructor(engine: Engine | undefined) {
    this.engine = engine;
    this.loader = vega.loader();
  }

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  async load(uri: string, _options?: any): Promise<string> {
    if (this.engine === undefined) {
      return '';
    }
    try {
      const result = await this.engine.query(uri);
      const columns = result.columns();
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      const rows: any[] = [];
      for (const it = result.iter({}); it.valid(); it.next()) {
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        const row: any = {};
        for (const name of columns) {
          let value = it.get(name);
          if (typeof value === 'bigint') {
            value = Number(value);
          }
          row[name] = value;
        }
        rows.push(row);
      }
      return JSON.stringify(rows);
    } catch (e) {
      if (e instanceof QueryError) {
        console.error(e);
        return '';
      } else {
        throw e;
      }
    }
  }

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  sanitize(uri: string, options: any): Promise<{href: string}> {
    return this.loader.sanitize(uri, options);
  }

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  http(uri: string, options: any): Promise<string> {
    return this.loader.http(uri, options);
  }

  file(filename: string): Promise<string> {
    return this.loader.file(filename);
  }
}

class VegaWrapper {
  private dom: Element;
  private _spec?: string;
  private _data?: VegaViewData;
  private view?: vega.View;
  private pending?: Promise<vega.View>;
  private _status: Status;
  private _error?: string;
  private _engine?: Engine;

  private _signalHandlers?: {
    readonly name: string;
    readonly handler: vega.SignalListenerHandler;
  }[];
  private _eventHandlers?: {
    readonly name: string;
    readonly handler: vega.EventListenerHandler;
  }[];
  private _onViewDestroyed?: () => void;

  constructor(dom: Element) {
    this.dom = dom;
    this._status = Status.Empty;
  }

  get status(): Status {
    return this._status;
  }

  get error(): string {
    return this._error ?? '';
  }

  set spec(value: string) {
    if (this._spec !== value) {
      this._spec = value;
      this.updateView();
    }
  }

  set data(value: VegaViewData) {
    if (this._data === value || shallowEquals(this._data, value)) {
      return;
    }
    this._data = value;
    this.updateView();
  }

  set engine(engine: Engine | undefined) {
    this._engine = engine;
  }

  set signalHandlers(
    handlers: {
      readonly name: string;
      readonly handler: VegaSignalListenerHandler;
    }[],
  ) {
    for (const {name, handler} of this._signalHandlers ?? []) {
      this.view?.removeSignalListener(name, handler);
    }
    this._signalHandlers = handlers.map(({name, handler}) => {
      return {
        name,
        handler: (name2, value) =>
          handler({view: this.view!, name: name2, value}),
      };
    });
    for (const {name, handler} of this._signalHandlers) {
      this.view?.addSignalListener(name, handler);
    }
  }

  set eventHandlers(
    handlers: {
      readonly name: string;
      readonly handler: VegaEventListenerHandler;
    }[],
  ) {
    for (const {name, handler} of this._eventHandlers ?? []) {
      this.view?.removeEventListener(name, handler);
    }
    this._eventHandlers = handlers.map(({name, handler}) => {
      return {
        name,
        handler: (event, item) => handler({view: this.view!, event, item}),
      };
    });
    for (const {name, handler} of this._eventHandlers) {
      this.view?.addEventListener(name, handler);
    }
  }

  set onViewDestroyed(handler: (() => void) | undefined) {
    this._onViewDestroyed = handler;
  }

  onResize() {
    if (this.view) {
      this.view.resize();
    }
  }

  private updateView() {
    this._status = Status.Empty;
    this._error = undefined;

    // We no longer care about inflight renders:
    if (this.pending) {
      this.pending = undefined;
    }

    // Destroy existing view if needed:
    if (this.view) {
      this._onViewDestroyed?.();
      this.view.finalize();
      this.view = undefined;
    }

    // If the spec and data are both available then create a new view:
    if (this._spec !== undefined && this._data !== undefined) {
      let spec;
      try {
        spec = JSON.parse(this._spec);
      } catch (e) {
        this.setError(e);
        return;
      }

      if (isVegaLite(spec)) {
        try {
          spec = vegaLite.compile(spec, {}).spec;
        } catch (e) {
          this.setError(e);
          return;
        }
      }

      // Create the runtime and view the bind the host DOM element
      // and any data.
      const runtime = vega.parse(spec);
      this.view = new vega.View(runtime, {
        loader: new EngineLoader(this._engine),
      });
      this.view.initialize(this.dom);
      for (const [key, value] of Object.entries(this._data)) {
        this.view.data(key, value);
      }

      if (isVegaLite(this._spec)) {
        for (const {name, handler} of this._signalHandlers ?? []) {
          this.view.addSignalListener(name, handler);
        }
        for (const {name, handler} of this._eventHandlers ?? []) {
          this.view.addEventListener(name, handler);
        }
      }

      const pending = this.view.runAsync();
      pending
        .then(() => {
          this.handleComplete(pending);
        })
        .catch((err) => {
          this.handleError(pending, err);
        });
      this.pending = pending;
      this._status = Status.Loading;
    }
  }

  private handleComplete(pending: Promise<vega.View>) {
    if (this.pending !== pending) {
      return;
    }
    this._status = Status.Done;
    this.pending = undefined;
    m.redraw();
  }

  private handleError(pending: Promise<vega.View>, err: unknown) {
    if (this.pending !== pending) {
      return;
    }
    this.pending = undefined;
    this.setError(err);
  }

  private setError(err: unknown) {
    this._status = Status.Error;
    this._error = getErrorMessage(err);
    m.redraw();
  }

  [Symbol.dispose]() {
    this._data = undefined;
    this._spec = undefined;
    this.updateView();
  }
}

export class VegaView implements m.ClassComponent<VegaViewAttrs> {
  private wrapper?: VegaWrapper;
  private resize?: Disposable;

  oncreate({dom, attrs}: m.CVnodeDOM<VegaViewAttrs>) {
    const wrapper = new VegaWrapper(dom.firstElementChild!);
    wrapper.spec = attrs.spec;
    wrapper.data = attrs.data;
    wrapper.engine = attrs.engine;
    wrapper.signalHandlers = attrs.signalHandlers ?? [];
    wrapper.eventHandlers = attrs.eventHandlers ?? [];
    wrapper.onViewDestroyed = attrs.onViewDestroyed;

    this.wrapper = wrapper;
    this.resize = new SimpleResizeObserver(dom, () => {
      wrapper.onResize();
    });
  }

  onupdate({attrs}: m.CVnodeDOM<VegaViewAttrs>) {
    if (this.wrapper) {
      this.wrapper.spec = attrs.spec;
      this.wrapper.data = attrs.data;
      this.wrapper.engine = attrs.engine;
      this.wrapper.signalHandlers = attrs.signalHandlers ?? [];
      this.wrapper.eventHandlers = attrs.eventHandlers ?? [];
      this.wrapper.onViewDestroyed = attrs.onViewDestroyed;
    }
  }

  onremove() {
    if (this.resize) {
      this.resize[Symbol.dispose]();
      this.resize = undefined;
    }
    if (this.wrapper) {
      this.wrapper[Symbol.dispose]();
      this.wrapper = undefined;
    }
  }

  view(_: m.Vnode<VegaViewAttrs>) {
    return m(
      '.pf-vega-view',
      m(''),
      this.wrapper?.status === Status.Loading &&
        m('.pf-vega-view-status', m(Spinner)),
      this.wrapper?.status === Status.Error &&
        m('.pf-vega-view-status', this.wrapper?.error ?? 'Error'),
    );
  }
}
