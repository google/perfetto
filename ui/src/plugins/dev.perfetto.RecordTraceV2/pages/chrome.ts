// Copyright (C) 2024 The Android Open Source Project
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
import {
  RecordSubpage,
  RecordProbe,
  ProbeSetting,
} from '../config/config_interfaces';
import {TraceConfigBuilder} from '../config/trace_config_builder';
import {Toggle} from './widgets/toggle';
import {Section} from '../../../widgets/section';
import {
  MultiSelect,
  MultiSelectDiff,
  MultiSelectOption,
} from '../../../widgets/multiselect';
import {Result} from '../../../base/result';

type ChromeCatFunction = () => Promise<Result<string[]>>;

export function chromeRecordSection(
  chromeCategoryGetter: ChromeCatFunction,
): RecordSubpage {
  return {
    kind: 'PROBES_PAGE',
    id: 'chrome',
    title: 'Chrome browser',
    subtitle: 'Chrome tracing',
    icon: 'laptop_chromebook',
    probes: [chromeProbe(chromeCategoryGetter)],
  };
}

function chromeProbe(chromeCategoryGetter: ChromeCatFunction): RecordProbe {
  const groupToggles = Object.fromEntries(
    Object.keys(GROUPS).map((groupName) => [
      groupName,
      new Toggle({
        title: groupName,
      }),
    ]),
  );
  const settings = {
    ...groupToggles,
    privacy: new Toggle({
      title: 'Remove untyped and sensitive data like URLs from the trace',
      descr:
        'Not recommended unless you intend to share the trace' +
        ' with third-parties.',
    }),
    categories: new ChromeCategoriesWidget(chromeCategoryGetter),
  };
  return {
    id: 'chrome_tracing',
    title: 'Chrome browser tracing',
    settings,
    genConfig: function (tc: TraceConfigBuilder) {
      const cats = new Set<string>();
      settings.categories.getEnabledCategories().forEach((c) => cats.add(c));
      for (const [group, groupCats] of Object.entries(GROUPS)) {
        if ((groupToggles[group] as Toggle).enabled) {
          groupCats.forEach((c) => cats.add(c));
        }
      }
      const memoryInfra = cats.has('disabled-by-default-memory-infra');
      const jsonStruct = {
        record_mode:
          tc.mode === 'STOP_WHEN_FULL'
            ? 'record-until-full'
            : 'record-continuously',
        included_categories: [...cats],
        excluded_categories: ['*'], // Only include categories explicitly
        memory_dump_config: memoryInfra
          ? {
              allowed_dump_modes: ['background', 'light', 'detailed'],
              triggers: [
                {
                  min_time_between_dumps_ms: 10000,
                  mode: 'detailed',
                  type: 'periodic_interval',
                },
              ],
            }
          : undefined,
      };
      const privacyFilteringEnabled = settings.privacy.enabled;
      const chromeConfig = {
        privacyFilteringEnabled,
        traceConfig: JSON.stringify(jsonStruct),
      };

      const trackEvent = tc.addDataSource('track_event');
      const trackEvtCfg = (trackEvent.trackEventConfig ??= {});
      trackEvtCfg.disabledCategories ??= ['*'];
      trackEvtCfg.enabledCategories ??= [];
      trackEvtCfg.enabledCategories.push(...cats);
      trackEvtCfg.enabledCategories.push('__metadata');
      trackEvtCfg.enableThreadTimeSampling = true;
      trackEvtCfg.timestampUnitMultiplier = 1000;
      trackEvtCfg.filterDynamicEventNames = privacyFilteringEnabled;
      trackEvtCfg.filterDebugAnnotations = privacyFilteringEnabled;

      tc.addBuffer('metadata', 256, 'DISCARD');
      tc.addDataSource(
        'org.chromium.trace_metadata2',
        'metadata',
      ).chromeConfig = {privacyFilteringEnabled};

      if (memoryInfra) {
        tc.addDataSource('org.chromium.memory_instrumentation').chromeConfig =
          chromeConfig;
        tc.addDataSource('org.chromium.native_heap_profiler').chromeConfig =
          chromeConfig;
      }

      if (
        cats.has('disabled-by-default-cpu_profiler') ||
        cats.has('disabled-by-default-cpu_profiler.debug')
      ) {
        tc.addDataSource('org.chromium.sampler_profiler').chromeConfig = {
          privacyFilteringEnabled,
        };
      }
      if (cats.has('disabled-by-default-system_metrics')) {
        tc.addDataSource('org.chromium.system_metrics');
      }
      if (cats.has('disabled-by-default-histogram_samples')) {
        const histogram = tc.addDataSource('org.chromium.histogram_sample');
        const histogramCfg = (histogram.chromiumHistogramSamples ??= {});
        histogramCfg.filterHistogramNames = privacyFilteringEnabled;
      }
    },
  };
}

const DISAB_PREFIX = 'disabled-by-default-';

export class ChromeCategoriesWidget implements ProbeSetting {
  private options = new Array<MultiSelectOption>();
  private fetchedRuntimeCategories = false;

  constructor(private chromeCategoryGetter: ChromeCatFunction) {
    // Initialize first with the static list of builtin categories (in case
    // something goes wrong with the extension).
    this.initializeCategories(BUILTIN_CATEGORIES);
  }

  private async fetchRuntimeCategoriesIfNeeded() {
    if (this.fetchedRuntimeCategories) return;
    const runtimeCategories = await this.chromeCategoryGetter();
    if (runtimeCategories.ok) {
      this.initializeCategories(runtimeCategories.value);
      m.redraw();
    }
    this.fetchedRuntimeCategories = true;
  }

  private initializeCategories(cats: string[]) {
    this.options = cats
      .map((cat) => ({
        id: cat,
        name: cat.replace(DISAB_PREFIX, ''),
        checked: this.options.find((o) => o.id === cat)?.checked ?? false,
      }))
      .sort((a, b) => a.name.toLowerCase().localeCompare(b.name.toLowerCase()));
  }

  getEnabledCategories(): string[] {
    return this.options.filter((o) => o.checked).map((o) => o.id);
  }

  setEnabled(cat: string, enabled: boolean) {
    for (const option of this.options) {
      if (option.id !== cat) continue;
      option.checked = enabled;
    }
  }

  serialize() {
    return this.options.filter((o) => o.checked).map((o) => o.id);
  }

  deserialize(state: unknown): void {
    if (Array.isArray(state) && state.every((x) => typeof x === 'string')) {
      this.options.forEach((o) => (o.checked = false));
      for (const key of state) {
        const opt = this.options.find((o) => o.id === key);
        if (opt !== undefined) opt.checked = true;
      }
    }
  }

  render() {
    return m(
      'div.chrome-categories',
      {
        // This shouldn't be necessary in most cases. It's only needed:
        // 1. The first time the user installs the extension.
        // 2. In rare cases if the extension fails to respond to the call in the
        //    constructor, to deal with its flakiness.
        oninit: () => this.fetchRuntimeCategoriesIfNeeded(),
      },
      m(
        Section,
        {title: 'Additional Categories'},
        m(MultiSelect, {
          options: this.options.filter((o) => !o.id.startsWith(DISAB_PREFIX)),
          repeatCheckedItemsAtTop: false,
          fixedSize: false,
          onChange: (diffs: MultiSelectDiff[]) => {
            diffs.forEach(({id, checked}) => this.setEnabled(id, checked));
          },
        }),
      ),
      m(
        Section,
        {title: 'High Overhead Categories'},
        m(MultiSelect, {
          options: this.options.filter((o) => o.id.startsWith(DISAB_PREFIX)),
          repeatCheckedItemsAtTop: false,
          fixedSize: false,
          onChange: (diffs: MultiSelectDiff[]) => {
            diffs.forEach(({id, checked}) => this.setEnabled(id, checked));
          },
        }),
      ),
    );
  }
}

function defaultAndDisabled(category: string) {
  return [category, 'disabled-by-default-' + category];
}

const GROUPS = {
  'Task Scheduling': [
    'toplevel',
    'toplevel.flow',
    'scheduler',
    'sequence_manager',
    'disabled-by-default-toplevel.flow',
  ],
  'IPC Flows': [
    'toplevel',
    'toplevel.flow',
    'disabled-by-default-ipc.flow',
    'mojom',
  ],
  'Javascript execution': ['toplevel', 'v8'],
  'Web content rendering, layout and compositing': [
    'toplevel',
    'blink',
    'cc',
    'gpu',
  ],
  'UI rendering and surface compositing': [
    'toplevel',
    'cc',
    'gpu',
    'viz',
    'ui',
    'views',
  ],
  'Input events': [
    'toplevel',
    'benchmark',
    'evdev',
    'input',
    'disabled-by-default-toplevel.flow',
  ],
  'Navigation and loading': [
    'loading',
    'net',
    'netlog',
    'navigation',
    'browser',
  ],
  'Audio': [
    'base',
    ...defaultAndDisabled('audio'),
    ...defaultAndDisabled('webaudio'),
    ...defaultAndDisabled('webaudio.audionode'),
    ...defaultAndDisabled('webrtc'),
    ...defaultAndDisabled('audio-worklet'),
    ...defaultAndDisabled('mediastream'),
    ...defaultAndDisabled('v8.gc'),
    ...defaultAndDisabled('toplevel'),
    ...defaultAndDisabled('toplevel.flow'),
    ...defaultAndDisabled('wakeup.flow'),
    ...defaultAndDisabled('cpu_profiler'),
    ...defaultAndDisabled('scheduler'),
    ...defaultAndDisabled('p2p'),
    ...defaultAndDisabled('net'),
  ],
  'Video': [
    'base',
    'gpu',
    'gpu.capture',
    'media',
    'toplevel',
    'toplevel.flow',
    'scheduler',
    'wakeup.flow',
    'webrtc',
    'disabled-by-default-video_and_image_capture',
    'disabled-by-default-webrtc',
  ],
};

// List of static Chrome categories, last updated at 2024-05-15 from HEAD of
// Chromium's //base/trace_event/builtin_categories.h.
const BUILTIN_CATEGORIES = [
  'accessibility',
  'AccountFetcherService',
  'android.adpf',
  'android.ui.jank',
  'android_webview',
  'android_webview.timeline',
  'aogh',
  'audio',
  'base',
  'benchmark',
  'blink',
  'blink.animations',
  'blink.bindings',
  'blink.console',
  'blink.net',
  'blink.resource',
  'blink.user_timing',
  'blink.worker',
  'blink_style',
  'Blob',
  'browser',
  'browsing_data',
  'CacheStorage',
  'Calculators',
  'CameraStream',
  'cppgc',
  'camera',
  'cast_app',
  'cast_perf_test',
  'cast.mdns',
  'cast.mdns.socket',
  'cast.stream',
  'cc',
  'cc.debug',
  'cdp.perf',
  'chromeos',
  'cma',
  'compositor',
  'content',
  'content_capture',
  'interactions',
  'delegated_ink_trails',
  'device',
  'devtools',
  'devtools.contrast',
  'devtools.timeline',
  'disk_cache',
  'download',
  'download_service',
  'drm',
  'drmcursor',
  'dwrite',
  'DXVA_Decoding',
  'evdev',
  'event',
  'event_latency',
  'exo',
  'extensions',
  'explore_sites',
  'FileSystem',
  'file_system_provider',
  'fledge',
  'fonts',
  'GAMEPAD',
  'gpu',
  'gpu.angle',
  'gpu.angle.texture_metrics',
  'gpu.capture',
  'graphics.pipeline',
  'headless',
  'history',
  'hwoverlays',
  'identity',
  'ime',
  'IndexedDB',
  'input',
  'input.scrolling',
  'io',
  'ipc',
  'Java',
  'jni',
  'jpeg',
  'latency',
  'latencyInfo',
  'leveldb',
  'loading',
  'log',
  'login',
  'media',
  'media_router',
  'memory',
  'midi',
  'mojom',
  'mus',
  'native',
  'navigation',
  'navigation.debug',
  'net',
  'network.scheduler',
  'netlog',
  'offline_pages',
  'omnibox',
  'oobe',
  'openscreen',
  'ozone',
  'partition_alloc',
  'passwords',
  'p2p',
  'page-serialization',
  'paint_preview',
  'pepper',
  'PlatformMalloc',
  'power',
  'ppapi',
  'ppapi_proxy',
  'print',
  'raf_investigation',
  'rail',
  'renderer',
  'renderer_host',
  'renderer.scheduler',
  'resources',
  'RLZ',
  'ServiceWorker',
  'SiteEngagement',
  'safe_browsing',
  'scheduler',
  'scheduler.long_tasks',
  'screenlock_monitor',
  'segmentation_platform',
  'sequence_manager',
  'service_manager',
  'sharing',
  'shell',
  'shortcut_viewer',
  'shutdown',
  'skia',
  'sql',
  'stadia_media',
  'stadia_rtc',
  'startup',
  'sync',
  'system_apps',
  'test_gpu',
  'toplevel',
  'toplevel.flow',
  'ui',
  'v8',
  'v8.execute',
  'v8.wasm',
  'ValueStoreFrontend::Backend',
  'views',
  'views.frame',
  'viz',
  'vk',
  'wakeup.flow',
  'wayland',
  'webaudio',
  'webengine.fidl',
  'weblayer',
  'WebCore',
  'webnn',
  'webrtc',
  'webrtc_stats',
  'xr',
  'disabled-by-default-android_view_hierarchy',
  'disabled-by-default-animation-worklet',
  'disabled-by-default-audio',
  'disabled-by-default-audio.latency',
  'disabled-by-default-audio-worklet',
  'disabled-by-default-base',
  'disabled-by-default-blink.debug',
  'disabled-by-default-blink.debug.display_lock',
  'disabled-by-default-blink.debug.layout',
  'disabled-by-default-blink.debug.layout.trees',
  'disabled-by-default-blink.feature_usage',
  'disabled-by-default-blink.image_decoding',
  'disabled-by-default-blink.invalidation',
  'disabled-by-default-identifiability',
  'disabled-by-default-identifiability.high_entropy_api',
  'disabled-by-default-cc',
  'disabled-by-default-cc.debug',
  'disabled-by-default-cc.debug.cdp-perf',
  'disabled-by-default-cc.debug.display_items',
  'disabled-by-default-cc.debug.lcd_text',
  'disabled-by-default-cc.debug.picture',
  'disabled-by-default-cc.debug.scheduler',
  'disabled-by-default-cc.debug.scheduler.frames',
  'disabled-by-default-cc.debug.scheduler.now',
  'disabled-by-default-content.verbose',
  'disabled-by-default-cpu_profiler',
  'disabled-by-default-cppgc',
  'disabled-by-default-cpu_profiler.debug',
  'disabled-by-default-devtools.screenshot',
  'disabled-by-default-devtools.timeline',
  'disabled-by-default-devtools.timeline.frame',
  'disabled-by-default-devtools.timeline.inputs',
  'disabled-by-default-devtools.timeline.invalidationTracking',
  'disabled-by-default-devtools.timeline.layers',
  'disabled-by-default-devtools.timeline.picture',
  'disabled-by-default-devtools.timeline.stack',
  'disabled-by-default-devtools.target-rundown',
  'disabled-by-default-devtools.v8-source-rundown',
  'disabled-by-default-devtools.v8-source-rundown-sources',
  'disabled-by-default-file',
  'disabled-by-default-fonts',
  'disabled-by-default-gpu_cmd_queue',
  'disabled-by-default-gpu.dawn',
  'disabled-by-default-gpu.debug',
  'disabled-by-default-gpu.decoder',
  'disabled-by-default-gpu.device',
  'disabled-by-default-gpu.graphite.dawn',
  'disabled-by-default-gpu.service',
  'disabled-by-default-gpu.vulkan.vma',
  'disabled-by-default-histogram_samples',
  'disabled-by-default-java-heap-profiler',
  'disabled-by-default-layer-element',
  'disabled-by-default-layout_shift.debug',
  'disabled-by-default-lifecycles',
  'disabled-by-default-loading',
  'disabled-by-default-mediastream',
  'disabled-by-default-memory-infra',
  'disabled-by-default-memory-infra.v8.code_stats',
  'disabled-by-default-mojom',
  'disabled-by-default-net',
  'disabled-by-default-network',
  'disabled-by-default-paint-worklet',
  'disabled-by-default-power',
  'disabled-by-default-renderer.scheduler',
  'disabled-by-default-renderer.scheduler.debug',
  'disabled-by-default-sequence_manager',
  'disabled-by-default-sequence_manager.debug',
  'disabled-by-default-sequence_manager.verbose_snapshots',
  'disabled-by-default-skia',
  'disabled-by-default-skia.gpu',
  'disabled-by-default-skia.gpu.cache',
  'disabled-by-default-skia.shaders',
  'disabled-by-default-skottie',
  'disabled-by-default-SyncFileSystem',
  'disabled-by-default-system_power',
  'disabled-by-default-system_stats',
  'disabled-by-default-thread_pool_diagnostics',
  'disabled-by-default-toplevel.ipc',
  'disabled-by-default-user_action_samples',
  'disabled-by-default-v8.compile',
  'disabled-by-default-v8.cpu_profiler',
  'disabled-by-default-v8.gc',
  'disabled-by-default-v8.gc_stats',
  'disabled-by-default-v8.ic_stats',
  'disabled-by-default-v8.inspector',
  'disabled-by-default-v8.runtime',
  'disabled-by-default-v8.runtime_stats',
  'disabled-by-default-v8.runtime_stats_sampling',
  'disabled-by-default-v8.stack_trace',
  'disabled-by-default-v8.turbofan',
  'disabled-by-default-v8.wasm.detailed',
  'disabled-by-default-v8.wasm.turbofan',
  'disabled-by-default-video_and_image_capture',
  'disabled-by-default-display.framedisplayed',
  'disabled-by-default-viz.gpu_composite_time',
  'disabled-by-default-viz.debug.overlay_planes',
  'disabled-by-default-viz.hit_testing_flow',
  'disabled-by-default-viz.overdraw',
  'disabled-by-default-viz.quads',
  'disabled-by-default-viz.surface_id_flow',
  'disabled-by-default-viz.surface_lifetime',
  'disabled-by-default-viz.triangles',
  'disabled-by-default-viz.visual_debugger',
  'disabled-by-default-webaudio.audionode',
  'disabled-by-default-webgpu',
  'disabled-by-default-webnn',
  'disabled-by-default-webrtc',
  'disabled-by-default-worker.scheduler',
  'disabled-by-default-xr.debug',
];
