// Copyright (C) 2022 The Android Open Source Project
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

import {assertExists} from '../base/logging';
import {Registry} from '../base/registry';
import {
  MetricVisualisation,
  PerfettoPlugin,
  PerfettoPluginStatic,
} from '../public/plugin';
import {Trace} from '../public/trace';
import {defaultPlugins} from './default_plugins';
import {featureFlags} from './feature_flags';
import {Flag} from '../public/feature_flag';
import {TraceImpl} from './trace_impl';
import {AppImpl} from './app_impl';
import {createProxy} from '../base/utils';
import {RouteArgs} from '../public/route_schema';
import {SettingsManagerImpl} from './settings_manager';
import {PageManager} from '../public/page';

function makePlugin(
  desc: PerfettoPluginStatic<PerfettoPlugin>,
  trace: Trace,
): PerfettoPlugin {
  const PluginClass = desc;
  return new PluginClass(trace);
}

// Contains information about a plugin.
export interface PluginWrapper {
  // A reference to the plugin descriptor
  readonly desc: PerfettoPluginStatic<PerfettoPlugin>;

  // The feature flag used to allow users to change whether this plugin should
  // be enabled or not.
  readonly enableFlag: Flag;

  // Record whether this plugin was enabled for this session, regardless of the
  // current flag setting. I.e. this captures the state of the enabled flag at
  // boot time.
  readonly enabled: boolean;

  // Whether this is a core plugin (part of CORE_PLUGINS) or not.
  readonly isCore: boolean;

  // Keeps track of whether this plugin is active. A plugin can be active even
  // if it's disabled, if another plugin depends on it.
  //
  // In summary, a plugin can be in one of three states:
  // - Inactive: Disabled and no active plugins depend on it.
  // - Transitively active: Disabled but active because another plugin depends
  //   on it.
  // - Explicitly active: Active because it was explicitly enabled by the user.
  active?: boolean;

  // If a trace has been loaded, this object stores the relevant trace-scoped
  // plugin data
  traceContext?: {
    // The concrete plugin instance, created on trace load.
    readonly instance: PerfettoPlugin;

    // How long it took for the plugin's onTraceLoad() function to run.
    readonly loadTimeMs: number;
  };
}

export class PluginManagerImpl {
  private readonly registry = new Registry<PluginWrapper>((x) => x.desc.id);
  private orderedPlugins: Array<PluginWrapper> = [];

  registerPlugin(desc: PerfettoPluginStatic<PerfettoPlugin>, isCore = false) {
    const flagId = `plugin_${desc.id}`;
    const name = `Plugin: ${desc.id}`;
    const flag = featureFlags.register({
      id: flagId,
      name,
      description: `Overrides '${desc.id}' plugin.`,
      defaultValue: defaultPlugins.includes(desc.id),
    });
    this.registry.register({
      desc,
      enableFlag: flag,
      enabled: flag.get(),
      isCore,
    });
  }

  /**
   * Activates all registered plugins that have not already been registered.
   *
   * @param app - The application instance.
   * @param enableOverrides - The list of plugins that are enabled regardless of
   * the current flag setting.
   */
  activatePlugins(app: AppImpl, enableOverrides: ReadonlyArray<string> = []) {
    const enabledPlugins = this.registry
      .valuesAsArray()
      .filter((p) => p.enableFlag.get() || enableOverrides.includes(p.desc.id));

    this.orderedPlugins = this.sortPluginsTopologically(enabledPlugins);

    this.orderedPlugins.forEach((p) => {
      if (p.active) return;
      const appProxy = createAppProxy(app, p.desc.id);
      const pluginArgs = getPluginArgs(app, p.desc.id);
      p.desc.onActivate?.(appProxy, pluginArgs);
      p.active = true;
    });
  }

  async onTraceLoad(
    trace: TraceImpl,
    beforeEach?: (id: string) => void,
  ): Promise<void> {
    // Awaiting all plugins in parallel will skew timing data as later plugins
    // will spend most of their time waiting for earlier plugins to load.
    // Running in parallel will have very little performance benefit assuming
    // most plugins use the same engine, which can only process one query at a
    // time.
    for (const p of this.orderedPlugins) {
      if (p.active) {
        beforeEach?.(p.desc.id);
        const traceProxy = createTraceProxy(trace, p.desc.id);
        const instance = makePlugin(p.desc, traceProxy);
        const args = getOpenerArgs(trace, p.desc.id);
        const before = performance.now();
        await instance.onTraceLoad?.(traceProxy, args);
        const loadTimeMs = performance.now() - before;
        p.traceContext = {
          instance,
          loadTimeMs,
        };
        trace.trash.defer(() => {
          p.traceContext = undefined;
        });
      }
    }
  }

  metricVisualisations(): MetricVisualisation[] {
    return this.registry.valuesAsArray().flatMap((plugin) => {
      if (!plugin.active) return [];
      return plugin.desc.metricVisualisations?.() ?? [];
    });
  }

  getAllPlugins() {
    return this.registry.valuesAsArray();
  }

  getPluginContainer(id: string): PluginWrapper | undefined {
    return this.registry.tryGet(id);
  }

  getPlugin<T extends PerfettoPlugin>(
    pluginDescriptor: PerfettoPluginStatic<T>,
  ): T {
    const plugin = this.registry.get(pluginDescriptor.id);
    return assertExists(plugin.traceContext).instance as T;
  }

  isCorePlugin(pluginId: string): boolean {
    const plugin = this.registry.tryGet(pluginId);
    return plugin?.isCore ?? false;
  }

  /**
   * Sort plugins in dependency order, ensuring that if a plugin depends on
   * other plugins, those plugins will appear fist in the list.
   */
  private sortPluginsTopologically(
    plugins: ReadonlyArray<PluginWrapper>,
  ): Array<PluginWrapper> {
    const orderedPlugins = new Array<PluginWrapper>();
    const visiting = new Set<string>();

    const visit = (p: PluginWrapper) => {
      // Continue if we've already added this plugin, there's no need to add it
      // again
      if (orderedPlugins.includes(p)) {
        return;
      }

      // Detect circular dependencies
      if (visiting.has(p.desc.id)) {
        const cycle = Array.from(visiting).concat(p.desc.id);
        throw new Error(
          `Cyclic plugin dependency detected: ${cycle.join(' -> ')}`,
        );
      }

      // Temporarily push this plugin onto the visiting stack while visiting
      // dependencies, to allow circular dependencies to be detected
      visiting.add(p.desc.id);

      // Recursively visit dependencies
      p.desc.dependencies?.forEach((d) => {
        visit(this.registry.get(d.id));
      });

      visiting.delete(p.desc.id);

      // Finally add this plugin to the ordered list
      orderedPlugins.push(p);
    };

    plugins.forEach((p) => visit(p));

    return orderedPlugins;
  }
}

/**
 * Creates a plugin-scoped proxy for the App instance.
 *
 * This proxy automatically injects the plugin's ID into any pages or settings
 * registered by the plugin, ensuring proper attribution and enabling cleanup
 * when the plugin is unloaded. It also recursively proxies the trace property
 * if one is loaded.
 */
function createAppProxy(app: AppImpl, pluginId: string): AppImpl {
  return createProxy(app, {
    get trace() {
      if (app.trace) {
        return createTraceProxy(app.trace, pluginId);
      } else {
        return undefined;
      }
    },
    get pages() {
      return createPagesProxy(app.pages, pluginId);
    },
    get settings() {
      return createSettingsProxy(app.settings, pluginId);
    },
  });
}

/**
 * Creates a plugin-scoped proxy for the Trace instance.
 *
 * This proxy automatically injects the plugin's ID into any pages, settings,
 * and tracks registered by the plugin. This ensures that all trace-scoped
 * resources created by the plugin are properly attributed and can be
 * automatically cleaned up when the trace is closed. It also proxies
 * the trace property back to itself.
 */
function createTraceProxy(trace: TraceImpl, pluginId: string): TraceImpl {
  const traceProxy = createProxy(trace, {
    get engine() {
      return trace.engine.getProxy(pluginId);
    },
    get trace(): TraceImpl {
      return traceProxy; // Return this proxy.
    },
    get pages() {
      return createPagesProxy(trace.pages, pluginId);
    },
    get settings() {
      return createSettingsProxy(trace.settings, pluginId);
    },
    get tracks() {
      return createProxy(trace.tracks, {
        registerTrack(track) {
          return trace.tracks.registerTrack({
            ...track,
            pluginId,
          });
        },
      });
    },
  });
  return traceProxy;
}

/**
 * Creates a proxy for the PageManager that automatically injects the pluginId
 * into any registered pages.
 */
function createPagesProxy<T extends PageManager>(
  pages: T,
  pluginId: string,
): T {
  return createProxy(pages, {
    registerPage(page) {
      return pages.registerPage({
        ...page,
        pluginId,
      });
    },
  } as Partial<T>);
}

/**
 * Creates a proxy for the SettingsManager that automatically injects the
 * pluginId into any registered settings.
 */
function createSettingsProxy<T extends SettingsManagerImpl>(
  settings: T,
  pluginId: string,
): T {
  return createProxy(settings, {
    register(setting) {
      return settings.register(setting, pluginId);
    },
  } as Partial<T>);
}

function getPluginArgs(app: AppImpl, pluginId: string): RouteArgs {
  return Object.entries(app.initialRouteArgs).reduce((result, [key, value]) => {
    // Create a regex to match keys starting with pluginId
    const regex = new RegExp(`^${pluginId}:(.+)$`);
    const match = key.match(regex);

    // Only include entries that match the regex
    if (match) {
      const newKey = match[1];
      // Use the capture group (what comes after the prefix) as the new key
      result[newKey] = value;
    }
    return result;
  }, {} as RouteArgs);
}

function getOpenerArgs(
  trace: TraceImpl,
  pluginId: string,
): {[key: string]: unknown} | undefined {
  const traceSource = trace.traceInfo.source;
  if (traceSource.type !== 'ARRAY_BUFFER') {
    return undefined;
  }
  const pluginArgs = traceSource.pluginArgs;
  return (pluginArgs ?? {})[pluginId];
}
