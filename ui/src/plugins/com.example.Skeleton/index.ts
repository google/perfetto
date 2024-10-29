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

import {Trace} from '../../public/trace';
import {App} from '../../public/app';
import {MetricVisualisation} from '../../public/plugin';
import {PerfettoPlugin} from '../../public/plugin';
import {createStore, Store} from '../../base/store';

interface State {
  foo: string;
}

// SKELETON: Rename this class to match your plugin.
export default class implements PerfettoPlugin {
  // SKELETON: Update pluginId to match the directory of the plugin.
  static readonly id = 'com.example.Skeleton';

  private store: Store<State> = createStore({foo: 'foo'});

  /**
   * This hook is called when the plugin is activated manually, or when the UI
   * starts up with this plugin enabled. This is typically before a trace has
   * been loaded, so there is no trace information in the passed plugin context
   * object.
   *
   * This hook should be used for adding commands that don't depend on the
   * trace.
   */
  static onActivate(_: App): void {
    //
  }

  /**
   * This hook is called as the trace is loading. At this point the trace is
   * loaded into trace processor and it's ready to process queries. This hook
   * should be used for adding tracks and commands that depend on the trace.
   *
   * It should not be used for finding tracks from other plugins as there is no
   * guarantee those tracks will have been added yet.
   */
  async onTraceLoad(ctx: Trace): Promise<void> {
    this.store = ctx.mountStore((_: unknown): State => {
      return {foo: 'bar'};
    });

    this.store.edit((state) => {
      state.foo = 'baz';
    });

    // This is an example of how to access the pluginArgs pushed by the
    // postMessage when deep-linking to the UI.
    if (ctx.openerPluginArgs !== undefined) {
      console.log(`Postmessage args for ${ctx.pluginId}`, ctx.openerPluginArgs);
    }

    /**
     * This hook is called when the trace has finished loading, and all plugins
     * have returned from their onTraceLoad calls. The UI can be considered
     * 'ready' at this point. All tracks and commands should now be available,
     * and the timeline is ready to use.
     *
     * This is where any automations should be done - things that you would
     * usually do manually after the trace has loaded but you'd like to automate
     * them.
     *
     * Examples of things that could be done here:
     * - Pinning tracks
     * - Focusing on a slice
     * - Adding debug tracks
     *
     * Postmessage args might be useful here - e.g. if you would like to pin a
     * specific track, pass the track details through the postmessage args
     * interface and react to it here.
     *
     * Note: Any tracks registered in this hook will not be displayed in the
     * timeline, unless they are manually added through the ctx.timeline API.
     * However this part of the code is in flux at the moment and the semantics
     * of how this works might change, though it's still good practice to use
     * the onTraceLoad hook to add tracks as it means that all tracks are
     * available by the time this hook gets called.
     *
     * TODO(stevegolton): Update this comment if the semantics of track adding
     * changes.
     */
    ctx.addEventListener('traceready', async () => {
      console.log('onTraceReady called');
    });
  }

  static metricVisualisations(): MetricVisualisation[] {
    return [];
  }
}
