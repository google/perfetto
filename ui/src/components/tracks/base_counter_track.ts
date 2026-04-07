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
import z from 'zod';
import {searchSegment} from '../../base/binary_search';
import {Point2D} from '../../base/geom';
import {assertTrue, assertUnreachable} from '../../base/assert';
import {Monitor} from '../../base/monitor';
import {duration, Time, time} from '../../base/time';
import {TimeScale} from '../../base/time_scale';
import {Trace} from '../../public/trace';
import {
  TrackMouseEvent,
  TrackRenderContext,
  TrackRenderer,
  TrackSetting,
  TrackSettingDescriptor,
} from '../../public/track';
import {LONG, NUM} from '../../trace_processor/query_result';
import {MenuItem} from '../../widgets/menu';
import {TextInput} from '../../widgets/text_input';
import {checkerboardExcept} from '../checkerboard';
import {valueIfAllEqual} from '../../base/array_utils';
import {deferChunkedTask} from '../../base/chunked_task';
import {CHUNKED_TASK_BACKGROUND_PRIORITY} from './feature_flags';
import {HSLColor} from '../../base/color';
import {
  CancellationSignal,
  QuerySlot,
  QUERY_CANCELLED,
  SerialTaskQueue,
} from '../../base/query_slot';
import {BufferedBounds} from './buffered_bounds';
import {createVirtualTable} from '../../trace_processor/sql_utils';

const BUCKETS_PER_PIXEL = 2;

// Returns a SQL expression that computes the display value from a table
// with `ts` and `value` columns, given the counter mode.
export function counterValueExpression(yMode: CounterOptions['yMode']): string {
  switch (yMode) {
    case 'value':
      return 'value';
    case 'delta':
      return 'lead(value, 1, value) over (order by ts) - value';
    case 'rate':
      return '(lead(value, 1, value) over (order by ts) - value) / ((lead(ts, 1, 100) over (order by ts) - ts) / 1e9)';
    default:
      assertUnreachable(yMode);
  }
}

// Returns the display label for a counter value given the mode.
export function counterDisplayLabel(yMode: CounterOptions['yMode']): string {
  switch (yMode) {
    case 'value':
      return 'Value';
    case 'delta':
      return 'Delta';
    case 'rate':
      return 'Rate';
    default:
      assertUnreachable(yMode);
  }
}

// Returns the unit string for a counter value given the mode.
export function counterDisplayUnit(
  yMode: CounterOptions['yMode'],
  unit: string,
  rateUnit: string,
): string {
  switch (yMode) {
    case 'value':
      return unit;
    case 'delta':
      return `\u0394${unit}`;
    case 'rate':
      return rateUnit;
    default:
      assertUnreachable(yMode);
  }
}

function roundAway(n: number): number {
  const exp = Math.ceil(Math.log10(Math.max(Math.abs(n), 1)));
  const pow10 = Math.pow(10, exp);
  return Math.sign(n) * (Math.ceil(Math.abs(n) / (pow10 / 20)) * (pow10 / 20));
}

function toLabel(n: number, exact = false): string {
  if (n === 0) {
    return '0';
  }
  const units: [number, string][] = [
    [0.000000001, 'n'],
    [0.000001, 'u'],
    [0.001, 'm'],
    [1, ''],
    [1000, 'K'],
    [1000 * 1000, 'M'],
    [1000 * 1000 * 1000, 'G'],
    [1000 * 1000 * 1000 * 1000, 'T'],
  ];
  let largestMultiplier = units[0][0];
  let largestUnit = units[0][1];
  const absN = Math.abs(n);
  for (const [multiplier, unit] of units) {
    if (multiplier > absN) {
      break;
    }
    largestMultiplier = multiplier;
    largestUnit = unit;
  }
  const value = n / largestMultiplier;

  if (!exact) {
    return `${Math.round(value)}${largestUnit}`;
  }

  if (Math.abs(value - Math.round(value)) < 1e-9) {
    return `${Math.round(value)}${largestUnit}`;
  }
  return `${Number(value.toFixed(1))}${largestUnit}`;
}

class RangeSharer {
  private static traceToRangeSharer = new WeakMap<Trace, RangeSharer>();

  private tagToRange: Map<string, [number, number]>;
  private keyToEnabled: Map<string, boolean>;

  constructor() {
    this.tagToRange = new Map();
    this.keyToEnabled = new Map();
  }

  static getRangeSharer(trace: Trace): RangeSharer {
    let sharer = RangeSharer.traceToRangeSharer.get(trace);
    if (sharer === undefined) {
      sharer = new RangeSharer();
      RangeSharer.traceToRangeSharer.set(trace, sharer);
    }
    return sharer;
  }

  isEnabled(key: string): boolean {
    const value = this.keyToEnabled.get(key);
    if (value === undefined) {
      return true;
    }
    return value;
  }

  setEnabled(key: string, enabled: boolean): void {
    this.keyToEnabled.set(key, enabled);
  }

  share(
    options: CounterOptions,
    [min, max]: [number, number],
  ): [number, number] {
    const key = options.yRangeSharingKey;
    if (key === undefined || !this.isEnabled(key)) {
      return [min, max];
    }

    const tag = `${options.yRangeSharingKey}-${options.yMode}-${
      options.yDisplay
    }-${options.chartHeightSize}`;
    const cachedRange = this.tagToRange.get(tag);
    if (cachedRange === undefined) {
      this.tagToRange.set(tag, [min, max]);
      return [min, max];
    }

    cachedRange[0] = Math.min(min, cachedRange[0]);
    cachedRange[1] = Math.max(max, cachedRange[1]);

    return [cachedRange[0], cachedRange[1]];
  }
}

interface CounterData {
  timestamps: BigInt64Array;
  minDisplayValues: Float64Array;
  maxDisplayValues: Float64Array;
  lastDisplayValues: Float64Array;
  displayValueRange: [number, number];
  // Relative timestamps for fast rendering (relative to dataStart)
  dataStart: time;
  dataEnd: time;
  timestampsRelNs: Float64Array;
}

// 0.5 Makes the horizontal lines sharp.
const MARGIN_TOP = 3.5;

interface CounterLimits {
  maxDisplayValue: number;
  minDisplayValue: number;
}

interface CounterTooltipState {
  lastDisplayValue: number;
  ts: time;
  tsEnd?: time;
}

function computeCounterHover(
  pos: Point2D | undefined,
  timescale: TimeScale,
  data: CounterData | undefined,
): CounterTooltipState | undefined {
  if (pos === undefined) return undefined;
  if (data === undefined || data.timestamps.length === 0) return undefined;

  const time = timescale.pxToHpTime(pos.x);
  const [left, right] = searchSegment(data.timestamps, time.toTime());
  if (left === -1) return undefined;

  return {
    ts: Time.fromRaw(data.timestamps[left]),
    tsEnd: right === -1 ? undefined : Time.fromRaw(data.timestamps[right]),
    lastDisplayValue: data.lastDisplayValues[left],
  };
}

type ChartHeightSize = 1 | 2 | 4 | 8 | 16 | 32;

const CHART_HEIGHT_LABELS: [string, ChartHeightSize][] = [
  ['Small (1x)', 1],
  ['Medium (2x)', 2],
  ['Large (4x)', 4],
  ['XLarge (8x)', 8],
  ['XXLarge (16x)', 16],
  ['XXXLarge (32x)', 32],
];

export interface CounterOptions {
  // Mode for computing the y value. Options are:
  // value = v[t] directly the value of the counter at time t
  // delta = v[t] - v[t-1] delta between value and previous value
  // rate = (v[t] - v[t-1]) / dt as delta but normalized for time
  yMode: 'value' | 'delta' | 'rate';

  // Whether Y scale should cover all of the possible values (and therefore, be
  // static) or whether it should be dynamic and cover only the visible values.
  yRange: 'all' | 'viewport';

  // Whether the Y scale should:
  // zero = y-axis scale should cover the origin (zero)
  // minmax = y-axis scale should cover just the range of yRange
  // log = as minmax but also use a log scale
  // custom = use explicit minimum and/or maximum bounds
  yDisplay: YDisplay;

  // Whether the range boundaries should be strict and use the precise min/max
  // values or whether they should be rounded down/up to the nearest human
  // readable value.
  yRangeRounding: 'strict' | 'human_readable';

  // Scales the height of the chart.
  chartHeightSize: ChartHeightSize;

  // Allows *extending* the range of the y-axis counter increasing
  // the maximum (via yOverrideMaximum) or decreasing the minimum
  // (via yOverrideMinimum). This is useful for percentage counters
  // where the range (0-100) is known statically upfront and even if
  // the trace only includes smaller values.
  yOverrideMaximum?: number;
  yOverrideMinimum?: number;

  // If set all counters with the same key share a range.
  yRangeSharingKey?: string;

  // unit for the counter. This is displayed in the tooltip and
  // legend.
  unit?: string;

  // unit to use when yMode is set to 'rate'. This rateUnit should be
  // equivalent to unit/s. For example, if the 'unit' is Joules, the 'rateUnit'
  // may be set to Watts. If not specified, unit/s will be used.
  rateUnit?: string;
}

const radioIconChecked = 'radio_button_checked';
const radioIconUnchecked = 'radio_button_unchecked';

const ymodeSchema = z.enum(['value', 'delta', 'rate']);
type yMode = z.infer<typeof ymodeSchema>;

const yRangeSchema = z.union([z.literal('all'), z.literal('viewport')]);
type YRange = z.infer<typeof yRangeSchema>;

const yDisplaySchema = z.discriminatedUnion('kind', [
  z.object({kind: z.literal('zero')}),
  z.object({kind: z.literal('minmax')}),
  z.object({kind: z.literal('log')}),
  z.object({
    kind: z.literal('custom'),
    min: z.number().optional(),
    max: z.number().optional(),
  }),
]);
type YDisplay = z.infer<typeof yDisplaySchema>;

function yDisplayEquals(a: YDisplay, b: YDisplay): boolean {
  if (a.kind !== b.kind) return false;
  if (a.kind !== 'custom' || b.kind !== 'custom') return true;
  return a.min === b.min && a.max === b.max;
}

function getUniformYDisplayValue(
  values: ReadonlyArray<YDisplay>,
): YDisplay | undefined {
  if (values.length === 0) return undefined;
  const first = values[0];
  for (let i = 1; i < values.length; i++) {
    if (!yDisplayEquals(first, values[i])) {
      return undefined;
    }
  }
  return first;
}

function yDisplayKindLabel(value: YDisplay | undefined): string {
  return value?.kind ?? 'mixed';
}

const yRangeRoundingSchema = z.union([
  z.literal('strict'),
  z.literal('human_readable'),
]);
type YRangeRounding = z.infer<typeof yRangeRoundingSchema>;

const chartHeightSizeSchema = z.union([
  z.literal(1),
  z.literal(2),
  z.literal(4),
  z.literal(8),
  z.literal(16),
  z.literal(32),
]);

const yModeSettingDescriptor: TrackSettingDescriptor<yMode> = {
  id: 'yMode',
  name: 'Mode',
  description: 'value, delta, rate',
  schema: ymodeSchema,
  defaultValue: 'value',
  render(setter, values) {
    const value = valueIfAllEqual(values);
    return m(MenuItem, {label: `Mode (currently: ${value ?? 'mixed'})`}, [
      m(MenuItem, {
        label: 'Value',
        onclick: () => setter('value'),
        icon: value === 'value' ? radioIconChecked : radioIconUnchecked,
      }),
      m(MenuItem, {
        label: 'Delta',
        onclick: () => setter('delta'),
        icon: value === 'delta' ? radioIconChecked : radioIconUnchecked,
      }),
      m(MenuItem, {
        label: 'Rate',
        onclick: () => setter('rate'),
        icon: value === 'rate' ? radioIconChecked : radioIconUnchecked,
      }),
    ]);
  },
};

const yRangeSettingDescriptor: TrackSettingDescriptor<YRange> = {
  id: 'yRange',
  name: 'Y-axis range',
  description: 'all, viewport',
  schema: yRangeSchema,
  defaultValue: 'all',
  render(setter, values) {
    const value = valueIfAllEqual(values);

    const icon = (() => {
      switch (value) {
        case 'viewport':
          return 'check_box';
        case 'all':
          return 'check_box_outline_blank';
        default:
          return 'indeterminate_check_box';
      }
    })();

    return m(MenuItem, {
      label: 'Zoom on scroll',
      icon,
      onclick: () => {
        switch (value) {
          case 'all':
            setter('viewport');
            break;
          case 'viewport':
          default:
            setter('all');
            break;
        }
      },
    });
  },
};

const yDisplaySettingDescriptor: TrackSettingDescriptor<YDisplay> = {
  id: 'yDisplay',
  name: 'Y-axis display',
  description: 'zero, minmax, log, custom',
  schema: yDisplaySchema,
  defaultValue: {kind: 'zero'},
  render(setter, values) {
    const value = getUniformYDisplayValue(values);
    const isCustom = value?.kind === 'custom';
    const customValue: Extract<YDisplay, {kind: 'custom'}> = isCustom
      ? value
      : {kind: 'custom'};

    return m(
      MenuItem,
      {label: `Display (currently: ${yDisplayKindLabel(value)})`},
      [
        m(MenuItem, {
          label: 'Zero-based',
          onclick: () => setter({kind: 'zero'}),
          icon: value?.kind === 'zero' ? radioIconChecked : radioIconUnchecked,
        }),
        m(MenuItem, {
          label: 'Min/Max',
          onclick: () => setter({kind: 'minmax'}),
          icon:
            value?.kind === 'minmax' ? radioIconChecked : radioIconUnchecked,
        }),
        m(MenuItem, {
          label: 'Log',
          onclick: () => setter({kind: 'log'}),
          icon: value?.kind === 'log' ? radioIconChecked : radioIconUnchecked,
        }),
        m(MenuItem, {
          label: 'Custom Range',
          onclick: () => setter(isCustom ? customValue : {kind: 'custom'}),
          icon: isCustom ? radioIconChecked : radioIconUnchecked,
          closePopupOnClick: false,
        }),
        isCustom &&
          m('.pf-counter-track__custom-range', [
            m('span.pf-counter-track__custom-range-label', 'Min'),
            m(TextInput, {
              type: 'text',
              placeholder: 'auto',
              value:
                customValue.min !== undefined ? String(customValue.min) : '',
              onChange: (v) => {
                const trimmed = v.trim();
                const parsed = Number(trimmed);
                setter({
                  ...customValue,
                  min:
                    trimmed === ''
                      ? undefined
                      : Number.isFinite(parsed)
                        ? parsed
                        : undefined,
                });
              },
            }),
            m('span.pf-counter-track__custom-range-label', 'Max'),
            m(TextInput, {
              type: 'text',
              placeholder: 'auto',
              value:
                customValue.max !== undefined ? String(customValue.max) : '',
              onChange: (v) => {
                const trimmed = v.trim();
                const parsed = Number(trimmed);
                setter({
                  ...customValue,
                  max:
                    trimmed === ''
                      ? undefined
                      : Number.isFinite(parsed)
                        ? parsed
                        : undefined,
                });
              },
            }),
          ]),
      ],
    );
  },
};

const yRangeRoundingSettingDescriptor: TrackSettingDescriptor<YRangeRounding> =
  {
    id: 'yRangeRounding',
    name: 'Y-axis rounding',
    description: 'strict, human_readable',
    schema: yRangeRoundingSchema,
    defaultValue: 'human_readable',
    render(setter, values) {
      const value = valueIfAllEqual(values);

      const icon = (() => {
        switch (value) {
          case 'human_readable':
            return 'check_box';
          case 'strict':
            return 'check_box_outline_blank';
          default:
            return 'indeterminate_check_box';
        }
      })();

      return m(MenuItem, {
        label: 'Round y-axis scale',
        icon,
        onclick: () => {
          switch (value) {
            case 'human_readable':
              setter('strict');
              break;
            case 'strict':
            default:
              setter('human_readable');
              break;
          }
        },
      });
    },
  };

const chartHeightSizeSettingDescriptor: TrackSettingDescriptor<ChartHeightSize> =
  {
    id: 'chartHeightSize',
    name: 'Chart height',
    description: '1, 4, 8, 16, 32',
    schema: chartHeightSizeSchema,
    defaultValue: 1,
    render(setter, values) {
      const value = valueIfAllEqual(values);
      return m(MenuItem, {label: `Size (currently: ${value ?? 'mixed'})`}, [
        CHART_HEIGHT_LABELS.map(([label, size]) =>
          m(MenuItem, {
            label,
            onclick: () => setter(size),
            icon: value === size ? radioIconChecked : radioIconUnchecked,
          }),
        ),
      ]);
    },
  };

function makeYRangeSharingDescriptor(
  key: string,
): TrackSettingDescriptor<boolean> {
  return {
    id: 'yRangeSharing',
    name: `Share y-axis scale (group: ${key})`,
    description: 'Share y-axis range with other tracks in the same group',
    schema: z.boolean(),
    defaultValue: false,
  };
}

// Result from mipmap table creation
interface MipmapTableResult extends AsyncDisposable {
  tableName: string;
  limits: CounterLimits;
}

// Result from data fetching - includes data and the reference time
interface CounterDataResult {
  data: CounterData;
  refStart: time;
}

export abstract class BaseCounterTrack implements TrackRenderer {
  // QuerySlot infrastructure
  private readonly queue = new SerialTaskQueue();
  private readonly initSlot = new QuerySlot<AsyncDisposable | void>(this.queue);
  private readonly tableSlot = new QuerySlot<MipmapTableResult>(this.queue);
  private readonly dataSlot = new QuerySlot<CounterDataResult>(this.queue);

  // Buffered bounds tracking
  private readonly bufferedBounds = new BufferedBounds();

  // Cached data for rendering
  private counters?: CounterData;
  private limits?: CounterLimits;

  private hover?: CounterTooltipState;
  private options?: CounterOptions;

  private readonly rangeSharer: RangeSharer;

  // Monitor for local hover state (triggers DOM redraw for tooltip).
  private readonly hoverMonitor = new Monitor([
    () => this.hover?.ts,
    () => this.hover?.lastDisplayValue,
  ]);

  getCounterOptions(): CounterOptions {
    if (this.options === undefined) {
      const options = this.getDefaultCounterOptions();
      for (const [key, value] of Object.entries(this.defaultOptions)) {
        if (value !== undefined) {
          // eslint-disable-next-line @typescript-eslint/no-explicit-any
          (options as any)[key] = value;
        }
      }
      this.options = options;
    }
    return this.options;
  }

  renderTooltip(): m.Children {
    if (this.hover) {
      const value =
        this.options?.yDisplay.kind === 'log'
          ? Math.exp(this.hover.lastDisplayValue)
          : this.hover.lastDisplayValue;

      return m('.pf-track__tooltip', this.formatValue(value));
    } else {
      return undefined;
    }
  }

  private formatValue(value: number) {
    const options = this.getCounterOptions();
    const unit = counterDisplayUnit(options.yMode, this.unit, this.rateUnit);
    return `${value.toLocaleString()} ${unit}`;
  }

  // Extension points.

  // onInit hook lets you do asynchronous set up e.g. creating a table
  // etc. We guarantee that this will be resolved before doing any
  // queries using the result of getSqlSource(). All persistent
  // state in trace_processor should be cleaned up when dispose is
  // called on the returned hook.
  async onInit(): Promise<AsyncDisposable | void> {}

  // This should be an SQL expression returning the columns `ts` and `value`.
  abstract getSqlSource(): string;

  protected getDefaultCounterOptions(): CounterOptions {
    return {
      yRange: 'all',
      yRangeRounding: 'human_readable',
      yMode: 'value',
      yDisplay: {kind: 'zero'},
      chartHeightSize: 1,
    };
  }

  constructor(
    protected readonly trace: Trace,
    protected readonly uri: string,
    protected readonly defaultOptions: Partial<CounterOptions> = {},
  ) {
    this.rangeSharer = RangeSharer.getRangeSharer(trace);
  }

  getHeight() {
    const height = 40;
    return height * this.getCounterOptions().chartHeightSize;
  }

  protected invalidate() {
    this.limits = undefined;
    this.counters = undefined;
    this.bufferedBounds.reset();
    this.hover = undefined;

    this.trace.raf.scheduleFullRedraw();
  }

  readonly yModeSetting: TrackSetting<yMode> = {
    descriptor: yModeSettingDescriptor,
    getValue: () => this.getCounterOptions().yMode,
    setValue: (yMode) => {
      this.options = {...this.getCounterOptions(), yMode};
      this.invalidate();
    },
  };

  readonly yRangeSetting: TrackSetting<YRange> = {
    descriptor: yRangeSettingDescriptor,
    getValue: () => this.getCounterOptions().yRange,
    setValue: (yRange) => {
      this.options = {...this.getCounterOptions(), yRange};
      this.invalidate();
    },
  };

  readonly yDisplaySetting: TrackSetting<YDisplay> = {
    descriptor: yDisplaySettingDescriptor,
    getValue: () => this.getCounterOptions().yDisplay,
    setValue: (yDisplay) => {
      this.options = {...this.getCounterOptions(), yDisplay};
      this.invalidate();
    },
  };

  readonly yRangeRoundingSetting: TrackSetting<YRangeRounding> = {
    descriptor: yRangeRoundingSettingDescriptor,
    getValue: () => this.getCounterOptions().yRangeRounding,
    setValue: (yRangeRounding) => {
      this.options = {...this.getCounterOptions(), yRangeRounding};
      this.invalidate();
    },
  };

  readonly chartHeightSizeSetting: TrackSetting<ChartHeightSize> = {
    descriptor: chartHeightSizeSettingDescriptor,
    getValue: () => this.getCounterOptions().chartHeightSize,
    setValue: (chartHeightSize) => {
      this.options = {...this.getCounterOptions(), chartHeightSize};
      this.invalidate();
    },
  };

  get settings(): ReadonlyArray<TrackSetting<unknown>> {
    const key = this.getCounterOptions().yRangeSharingKey;
    return [
      this.yDisplaySetting,
      this.chartHeightSizeSetting,
      this.yRangeSetting,
      ...(key !== undefined
        ? [
            {
              descriptor: makeYRangeSharingDescriptor(key),
              getValue: () => this.rangeSharer.isEnabled(key),
              setValue: (v: boolean) => {
                this.rangeSharer.setEnabled(key, v);
                this.invalidate();
              },
            },
          ]
        : []),
      this.yModeSetting,
      this.yRangeRoundingSetting,
    ];
  }

  /**
   * Declaratively fetches data for the track. Updates internal state
   * (counters, limits) when data is available.
   * @returns true if data is ready for rendering, false otherwise
   */
  private useData(trackCtx: TrackRenderContext): boolean {
    const {size, visibleWindow} = trackCtx;

    // Step 0: Call onInit with a constant key
    const initResult = this.initSlot.use({
      key: {init: true},
      queryFn: () => this.onInit(),
    });

    if (initResult.isPending) {
      return false;
    }

    // Step 1: Get the mipmap table (created once per SQL source + options)
    // Include yMode and yDisplay in key since they affect the value expression
    const options = this.getCounterOptions();
    const tableResult = this.tableSlot.use({
      key: {
        sqlSource: this.getSqlSource(),
        yMode: options.yMode,
        yDisplay: options.yDisplay,
      },
      queryFn: () => this.createMipmapTable(),
    });

    const table = tableResult.data;
    if (table === undefined) return false;

    // Update limits from table result
    this.limits = table.limits;

    // Step 2: Calculate buffered bounds and fetch counter data
    const visibleSpan = visibleWindow.toTimeSpan();
    const windowSizePx = Math.max(1, size.width);
    const bucketSize = this.computeBucketSize(
      visibleSpan.duration,
      windowSizePx,
    );
    const bounds = this.bufferedBounds.update(visibleSpan, bucketSize);

    // Step 3: Fetch counter data using QuerySlot
    const queryStart = bounds.start;
    const queryEnd = bounds.end;
    const dataResult = this.dataSlot.use({
      key: {
        start: bounds.start,
        end: bounds.end,
        resolution: bounds.resolution,
        yMode: options.yMode,
        yDisplay: options.yDisplay,
      },
      queryFn: async (signal) => {
        const result = await this.trace.taskTracker.track(
          this.fetchCounterData(
            table.tableName,
            queryStart,
            queryEnd,
            bounds.resolution,
            signal,
          ),
          'Loading counters',
        );
        this.trace.raf.scheduleFullRedraw();
        return {data: result, refStart: queryStart};
      },
      retainOn: ['start', 'end', 'resolution'],
    });

    // Update counters when new data arrives
    if (dataResult.data !== undefined) {
      this.counters = dataResult.data.data;
    }

    // Return true if we have data to render
    return this.counters !== undefined && this.limits !== undefined;
  }

  render(trackCtx: TrackRenderContext): void {
    const {ctx, size, timescale, colors, renderer} = trackCtx;

    // Fetch data declaratively - updates internal state
    if (!this.useData(trackCtx)) {
      return; // No data ready yet
    }

    const limits = this.limits!;
    const data = this.counters!;

    assertTrue(data.timestamps.length === data.minDisplayValues.length);
    assertTrue(data.timestamps.length === data.maxDisplayValues.length);
    assertTrue(data.timestamps.length === data.lastDisplayValues.length);

    const timestamps = data.timestamps;
    const minValues = data.minDisplayValues;
    const maxValues = data.maxDisplayValues;
    const lastValues = data.lastDisplayValues;

    // Choose a range for the y-axis
    const {yRange, yMin, yMax, yLabel} = this.computeYRange(
      limits,
      data.displayValueRange,
    );

    const effectiveHeight = this.getHeight() - MARGIN_TOP;
    const endPx = size.width;

    // Use hue to differentiate the scale of the counter value
    const exp = Math.ceil(Math.log10(Math.max(yMax, 1)));
    const expCapped = Math.min(exp - 3, 9);
    const hue = (180 - Math.floor(expCapped * (180 / 6)) + 360) % 360;

    const fillColor = new HSLColor([hue, 45, 50], 0.6);

    // Pre-compute conversion factors for fast timestamp-to-pixel conversion.
    const pxPerNs = timescale.durationToPx(1n);
    const baseOffsetPx = timescale.timeToPx(data.dataStart);
    const frameEndRelNs = Number(data.dataEnd - data.dataStart);

    const calculateX = (relNs: number) => {
      return Math.floor(relNs * pxPerNs + baseOffsetPx);
    };
    let zeroY;
    if (yMin >= 0) {
      zeroY = effectiveHeight + MARGIN_TOP;
    } else if (yMax < 0) {
      zeroY = MARGIN_TOP;
    } else {
      zeroY = effectiveHeight * (yMax / (yMax - yMin)) + MARGIN_TOP;
    }

    // Draw the counter graph using the renderer
    const count = timestamps.length;
    if (count >= 1) {
      // Pass raw data values - transform converts to screen coordinates This
      // could be a lot more efficient if we allocated these buffers when the
      // data changes and reused them every render cycle.
      const xs = new Float32Array(count);
      const ys = new Float32Array(count);
      const minYs = new Float32Array(count);
      const maxYs = new Float32Array(count);
      const fillAlpha = new Float32Array(count);
      const xnext = new Float32Array(count);

      for (let i = 0; i < count; i++) {
        xs[i] = Math.max(0, data.timestampsRelNs[i]); // Clamp to the start of the frame
        ys[i] = lastValues[i];
        minYs[i] = minValues[i];
        maxYs[i] = maxValues[i];
        fillAlpha[i] = 1.0;
        if (i > 0) {
          xnext[i - 1] = xs[i];
        }
      }

      // Final xnext is the end of the frame
      xnext[count - 1] = frameEndRelNs;

      // Build transform: raw data -> screen coordinates
      // X: screenX = relNs * pxPerNs + baseOffsetPx
      // Y: screenY = value * scaleY + offsetY (where y=0 maps to zeroY)
      const transform = {
        offsetX: baseOffsetPx,
        scaleX: pxPerNs,
        offsetY: zeroY,
        scaleY: -effectiveHeight / yRange,
      };

      renderer.drawStepArea(
        {xs, ys, minYs, maxYs, fillAlpha, count, xnext},
        transform,
        fillColor,
        MARGIN_TOP,
        this.getHeight(),
      );
    }

    if (yMin < 0 && yMax > 0) {
      // Draw the Y=0 dashed line.
      ctx.strokeStyle = `hsl(${hue}, 10%, 71%)`;
      ctx.beginPath();
      ctx.setLineDash([2, 4]);
      ctx.moveTo(0, zeroY);
      ctx.lineTo(endPx, zeroY);
      ctx.closePath();
      ctx.stroke();
      ctx.setLineDash([]);
    }
    ctx.font = '10px Roboto Condensed';

    const hover = this.hover;
    if (hover !== undefined) {
      ctx.fillStyle = `hsl(${hue}, 45%, 75%)`;
      ctx.strokeStyle = `hsl(${hue}, 45%, 45%)`;

      // Convert hover timestamps to relative for calculateX
      const hoverRelNs = Number(hover.ts - data.dataStart);
      const rawXStart = calculateX(hoverRelNs);
      const xStart = Math.max(0, rawXStart);
      const xEnd =
        hover.tsEnd === undefined
          ? endPx
          : calculateX(Number(hover.tsEnd - data.dataStart));
      const y =
        MARGIN_TOP +
        effectiveHeight -
        Math.round(
          ((hover.lastDisplayValue - yMin) / yRange) * effectiveHeight,
        );

      // Highlight line.
      ctx.beginPath();
      ctx.moveTo(xStart, y);
      ctx.lineTo(xEnd, y);
      ctx.lineWidth = 3;
      ctx.stroke();
      ctx.lineWidth = 1;

      // Draw change marker if it would be visible.
      if (rawXStart >= -6) {
        ctx.beginPath();
        ctx.arc(
          xStart,
          y,
          3 /* r*/,
          0 /* start angle*/,
          2 * Math.PI /* end angle*/,
        );
        ctx.fill();
        ctx.stroke();
      }
    }

    // Write the Y scale on the top left corner.
    ctx.textBaseline = 'alphabetic';
    ctx.fillStyle = colors.COLOR_BACKGROUND;
    ctx.globalAlpha = 0.6;
    ctx.fillRect(0, 0, 42, 18);
    ctx.globalAlpha = 1;
    ctx.fillStyle = colors.COLOR_TEXT;
    ctx.textAlign = 'left';
    ctx.fillText(`${yLabel}`, 4, 14);

    // TODO(hjd): Refactor this into checkerboardExcept
    {
      const counterEndPx = Infinity;
      // Grey out RHS.
      if (counterEndPx < endPx) {
        ctx.fillStyle = '#0000001f';
        ctx.fillRect(counterEndPx, 0, endPx - counterEndPx, this.getHeight());
      }
    }

    // If the cached trace slices don't fully cover the visible time range,
    // show a gray rectangle with a "Loading..." label.
    const loadedBounds = this.bufferedBounds.bounds;
    checkerboardExcept(
      ctx,
      this.getHeight(),
      0,
      size.width,
      timescale.timeToPx(loadedBounds.start),
      timescale.timeToPx(loadedBounds.end),
    );
  }

  onMouseMove({x, y, timescale}: TrackMouseEvent) {
    this.hover = computeCounterHover({x, y}, timescale, this.counters);
    if (this.hoverMonitor.ifStateChanged()) {
      this.trace.raf.scheduleFullRedraw();
    }
  }

  onMouseOut() {
    this.hover = undefined;
    if (this.hoverMonitor.ifStateChanged()) {
      this.trace.raf.scheduleFullRedraw();
    }
  }

  // Compute the range of values to display and range label.
  private computeYRange(
    limits: CounterLimits,
    dataLimits: [number, number],
  ): {
    yMin: number;
    yMax: number;
    yRange: number;
    yLabel: string;
  } {
    const options = this.getCounterOptions();

    let yMin = limits.minDisplayValue;
    let yMax = limits.maxDisplayValue;

    if (options.yRange === 'viewport') {
      [yMin, yMax] = dataLimits;
    }

    const yDisplay = options.yDisplay;

    if (yDisplay.kind === 'zero') {
      yMin = Math.min(0, yMin);
      yMax = Math.max(0, yMax);
    }

    if (yDisplay.kind === 'custom') {
      if (yDisplay.min !== undefined) yMin = yDisplay.min;
      if (yDisplay.max !== undefined) yMax = yDisplay.max;
    }

    if (options.yOverrideMaximum !== undefined) {
      yMax = Math.max(options.yOverrideMaximum, yMax);
    }

    if (options.yOverrideMinimum !== undefined) {
      yMin = Math.min(options.yOverrideMinimum, yMin);
    }

    // Skip rounding when the user has specified an exact custom range.
    if (
      options.yRangeRounding === 'human_readable' &&
      yDisplay.kind !== 'custom'
    ) {
      if (yDisplay.kind === 'log') {
        yMax = Math.log(roundAway(Math.exp(yMax)));
        yMin = Math.log(roundAway(Math.exp(yMin)));
      } else {
        yMax = roundAway(yMax);
        yMin = roundAway(yMin);
      }
    }

    // Ensure yMax > yMin to prevent division by zero in the renderer.
    // For non-custom modes we want strict behavior from data-driven bounds,
    // while custom range should be clamped to avoid invalid zero-size ranges.
    if (yDisplay.kind === 'custom' && yMax <= yMin) {
      yMax = yMin + 1;
    }

    [yMin, yMax] = this.rangeSharer.share(options, [yMin, yMax]);

    let yLabel: string;

    if (yDisplay.kind === 'minmax') {
      yLabel = 'min - max';
    } else {
      // For all dynamic modes, show difference. Prefer an exact label for
      // custom mode so 1,500 becomes 1.5K instead of rounding to 2K.
      let max = yMax;
      let min = yMin;
      if (yDisplay.kind === 'log') {
        max = Math.exp(max);
        min = Math.exp(min);
      }

      if (yDisplay.kind === 'custom') {
        const delta = max - min;
        yLabel = toLabel(delta, true);
      } else {
        const delta = max < 0 ? min - max : max - min;
        yLabel = toLabel(delta);
      }
    }

    const unit = this.unit;
    switch (options.yMode) {
      case 'value':
        yLabel += ` ${unit}`;
        break;
      case 'delta':
        yLabel += `\u0394${unit}`;
        break;
      case 'rate':
        yLabel += ` ${this.rateUnit}`;
        break;
      default:
        assertUnreachable(options.yMode);
    }

    if (yDisplay.kind === 'log') {
      yLabel = `log(${yLabel})`;
    }

    return {
      yMin,
      yMax,
      yLabel,
      yRange: yMax - yMin,
    };
  }

  // The underlying table has `ts` and `value` columns.
  private getValueExpression(): string {
    const options = this.getCounterOptions();
    const valueExpr = counterValueExpression(options.yMode);

    if (options.yDisplay.kind === 'log') {
      return `ifnull(ln(${valueExpr}), 0)`;
    } else {
      return valueExpr;
    }
  }

  // Compute bucket size for a given time span and pixel width
  private computeBucketSize(
    spanDuration: duration,
    windowSizePx: number,
  ): duration {
    const nsPerPx = Math.max(1, Number(spanDuration) / windowSizePx);
    const bucketNs = nsPerPx / BUCKETS_PER_PIXEL;
    const exp = Math.ceil(Math.log2(Math.max(1, bucketNs)));
    return BigInt(Math.pow(2, exp)) as duration;
  }

  // Creates the mipmap table - called declaratively from render via QuerySlot
  private async createMipmapTable(): Promise<MipmapTableResult> {
    const table = await createVirtualTable({
      engine: this.engine,
      using: `__intrinsic_counter_mipmap((
        select
          ts,
          ${this.getValueExpression()} as value
        from (${this.getSqlSource()})
      ))`,
    });

    // Fetch the global limits
    const limitsQuery = await this.engine.query(`
      select
        min_value as minDisplayValue,
        max_value as maxDisplayValue
      from ${table.name}(
        trace_start(), trace_end() + 1, trace_dur() + 1
      );
    `);

    const {minDisplayValue, maxDisplayValue} = limitsQuery.firstRow({
      minDisplayValue: NUM,
      maxDisplayValue: NUM,
    });

    return {
      tableName: table.name,
      limits: {minDisplayValue, maxDisplayValue},
      [Symbol.asyncDispose]: async () => {
        await table[Symbol.asyncDispose]();
      },
    };
  }

  // Fetches counter data for the given bounds - called from QuerySlot
  private async fetchCounterData(
    tableName: string,
    start: time,
    end: time,
    resolution: duration,
    signal: CancellationSignal,
  ): Promise<CounterData> {
    const queryRes = await this.engine.query(`
      SELECT
        min_value as minDisplayValue,
        max_value as maxDisplayValue,
        last_ts as ts,
        last_value as lastDisplayValue
      FROM ${tableName}(
        ${start},
        ${end},
        ${resolution}
      );
    `);

    if (signal.isCancelled) throw QUERY_CANCELLED;

    const priority = CHUNKED_TASK_BACKGROUND_PRIORITY.get()
      ? 'background'
      : undefined;
    const task = await deferChunkedTask({priority});

    const it = queryRes.iter({
      ts: LONG,
      minDisplayValue: NUM,
      maxDisplayValue: NUM,
      lastDisplayValue: NUM,
    });

    const numRows = queryRes.numRows();
    const data: CounterData = {
      timestamps: new BigInt64Array(numRows),
      minDisplayValues: new Float64Array(numRows),
      maxDisplayValues: new Float64Array(numRows),
      lastDisplayValues: new Float64Array(numRows),
      displayValueRange: [0, 0],
      dataStart: start,
      dataEnd: end,
      timestampsRelNs: new Float64Array(numRows),
    };

    let min = 0;
    let max = 0;
    for (let row = 0; it.valid(); it.next(), row++) {
      if (signal.isCancelled) throw QUERY_CANCELLED;
      if (row % 50 === 0 && task.shouldYield()) {
        await task.yield();
      }

      data.timestamps[row] = Time.fromRaw(it.ts);
      data.timestampsRelNs[row] = Number(it.ts - start);
      data.minDisplayValues[row] = it.minDisplayValue;
      data.maxDisplayValues[row] = it.maxDisplayValue;
      data.lastDisplayValues[row] = it.lastDisplayValue;
      min = Math.min(min, it.minDisplayValue);
      max = Math.max(max, it.maxDisplayValue);
    }

    data.displayValueRange = [min, max];
    return data;
  }

  get unit(): string {
    return this.getCounterOptions().unit ?? '';
  }

  get rateUnit(): string {
    return this.getCounterOptions().rateUnit ?? `\u0394${this.unit}/s`;
  }

  protected get engine() {
    return this.trace.engine;
  }
}
