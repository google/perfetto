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

// Keep this import first.
import '../base/static_initializers';
import m from 'mithril';
import {defer} from '../base/deferred';
import {reportError, addErrorHandler, ErrorDetails} from '../base/logging';
import {initLiveReload} from '../core/live_reload';

function getRoot() {
  // Works out the root directory where the content should be served from
  // e.g. `http://origin/v1.2.3/`.
  const script = document.currentScript as HTMLScriptElement;

  // Needed for DOM tests, that do not have script element.
  if (script === null) {
    return '';
  }

  let root = script.src;
  root = root.substr(0, root.lastIndexOf('/') + 1);
  return root;
}

function setupContentSecurityPolicy() {
  // Note: self and sha-xxx must be quoted, urls data: and blob: must not.
  const policy = {
    'default-src': [`'self'`],
    'script-src': [`'self'`],
    'object-src': ['none'],
    'connect-src': [`'self'`],
    'img-src': [`'self'`, 'data:', 'blob:'],
    'style-src': [`'self'`],
  };
  const meta = document.createElement('meta');
  meta.httpEquiv = 'Content-Security-Policy';
  let policyStr = '';
  for (const [key, list] of Object.entries(policy)) {
    policyStr += `${key} ${list.join(' ')}; `;
  }
  meta.content = policyStr;
  document.head.appendChild(meta);
}

function main() {
  setupContentSecurityPolicy();

  // Load the css. The load is asynchronous and the CSS is not ready by the time
  // appendChild returns.
  const root = getRoot();
  const cssLoadPromise = defer<void>();
  const css = document.createElement('link');
  css.rel = 'stylesheet';
  css.href = root + 'perfetto.css';
  css.onload = () => cssLoadPromise.resolve();
  css.onerror = (err) => cssLoadPromise.reject(err);
  const favicon = document.head.querySelector('#favicon') as HTMLLinkElement;
  if (favicon) favicon.href = root + 'assets/favicon.png';

  document.head.append(css);

  // Add Error handlers for JS error and for uncaught exceptions in promises.
  addErrorHandler((err: ErrorDetails) => console.log(err.message, err.stack));
  window.addEventListener('error', (e) => reportError(e));
  window.addEventListener('unhandledrejection', (e) => reportError(e));

  // Prevent pinch zoom.
  document.body.addEventListener(
    'wheel',
    (e: MouseEvent) => {
      if (e.ctrlKey) e.preventDefault();
    },
    {passive: false},
  );

  cssLoadPromise.then(() => onCssLoaded());
}

import {D3BarChartComponent, D3HistogramComponent, D3CDFComponent, D3ScatterChartComponent, D3HeatmapChartComponent, D3BoxplotChartComponent, D3ViolinPlotChartComponent, D3LineChartComponent, D3DonutChartComponent, D3StackedBarChartComponent, D3AreaChartComponent, FilterManager, DataTableComponent} from './d3';
import {InMemoryDataSource} from '../components/widgets/data_grid/in_memory_data_source';
import {raf} from '../core/raf_scheduler';
import {ThemeProvider} from '../frontend/theme_provider';
import {OverlayContainer} from '../widgets/overlay_container';

interface BigTraceState {
  dataSources: {[key: string]: {data: any[], columns: string[]}};
  currentDataSourceKey: string;
  set(updater: (draft: BigTraceState) => void): void;
}

function generateComprehensiveTestData(numRows = 100): any[] {
  const categories = ["Web", "Mobile", "API", "Desktop", "IoT", "Analytics"];
  const data = [];
  for (let i = 0; i < numRows; i++) {
    for (const category of categories) {
        data.push({
            category: category,
            value: Math.random() * 100,
            timestamp: i,
        });
    }
  }
  return data;
}

function generateSecondTestData(numRows = 100): any[] {
  const eventTypes = ["DB Query", "File IO", "Network Request", "UI Render"];
  const threads = ["main", "worker-1", "worker-2"];
  const data = [];
  for (let i = 0; i < numRows; i++) {
    data.push({
        event_type: eventTypes[i % eventTypes.length],
        duration_ms: Math.random() * 50,
        thread: threads[i % threads.length],
        ts: Date.now() + i * 1000,
    });
  }
  return data;
}

const state: BigTraceState = {
  dataSources: {
    'Schema 1': {
      data: generateComprehensiveTestData(100),
      columns: ['category', 'value', 'timestamp'],
    },
    'Schema 2': {
      data: generateSecondTestData(100),
      columns: ['event_type', 'duration_ms', 'thread', 'ts'],
    },
  },
  currentDataSourceKey: 'Schema 1',
  set(updater: (draft: BigTraceState) => void) {
    updater(this);
    m.redraw();
  },
};

function loadSchemaFromFile(file: File) {
  const reader = new FileReader();
  reader.onload = (event) => {
    const tsvContent = event.target?.result as string;
    console.log('Attempting to parse TSV from file:', file.name);
    try {
      const lines = tsvContent.trim().split('\n');
      if (lines.length < 2) {
        alert('TSV file must have a header and at least one data row.');
        return;
      }
      const headers = lines[0].split('\t');
      const data: {[key: string]: any}[] = [];
      for (let i = 1; i < lines.length; i++) {
        const values = lines[i].split('\t');
        if (values.length !== headers.length) {
          console.warn(`Skipping row ${i} due to column mismatch.`);
          continue;
        }
        const rowObject: {[key: string]: any} = {};
        headers.forEach((header, index) => {
          rowObject[header] = values[index];
        });
        data.push(rowObject);
      }

      const schemaName = file.name.replace(/\.tsv$/, '');
      state.set(draft => {
        draft.dataSources[schemaName] = {
          data: data,
          columns: headers,
        };
        switchDataSource(schemaName);
      });
      console.log(`Successfully loaded schema "${schemaName}" with ${data.length} rows and columns:`, headers);
    } catch (e) {
      alert('Error parsing TSV file.');
      console.error(e);
    }
  };
  reader.readAsText(file);
}

import {MemoryDataProvider} from './d3';
let mdp = new MemoryDataProvider(state.currentDataSourceKey in state.dataSources ? state.dataSources[state.currentDataSourceKey].data : []);
const filterManager = new FilterManager();
filterManager.setDataProvider(mdp);

function switchDataSource(newKey: string) {
  state.set(draft => {
    draft.currentDataSourceKey = newKey;
  });
  const newDataSource = state.dataSources[newKey];

  mdp = new MemoryDataProvider(newDataSource.data);

  filterManager.setDataProvider(mdp);
  filterManager.clearAllFilters();

  chartCreatorState.columns = newDataSource.columns;
  chartCreatorState.reset();

  chartState.charts = [];
}

// Chart configuration types
interface ChartConfig {
  id: string;
  type: string;
  xColumn?: string;
  yColumn?: string;
  valueColumn?: string;
  colorBy?: string;
  stackColumn?: string;
  categoryColumn?: string;
  aggregation?: string;
}

// Chart state management
const chartState = {
  charts: [] as ChartConfig[],
  nextId: 0,
  addChart(config: Omit<ChartConfig, 'id'>) {
    this.charts.push({
      ...config,
      id: `chart-${this.nextId++}`,
    });
    m.redraw();
  },
  removeChart(id: string) {
    this.charts = this.charts.filter(c => c.id !== id);
    m.redraw();
  },
};

// Sidebar states
const leftSidebarState = {
  collapsed: true,
  toggle() {
    this.collapsed = !this.collapsed;
    m.redraw();
  },
};

const rightSidebarState = {
  collapsed: false,
  toggle() {
    this.collapsed = !this.collapsed;
    m.redraw();
  },
};

// Chart creator state
const chartCreatorState = {
  schemaSearchTerm: '',
  chartTypeSearchTerm: '',
  selectedType: '',
  xColumn: '',
  yColumn: '',
  valueColumn: '',
  colorBy: '',
  stackColumn: '',
  categoryColumn: '',
  aggregation: 'sum',

  // Available options
  chartTypes: [
    { value: 'bar', label: 'Bar Chart' },
    { value: 'histogram', label: 'Histogram' },
    { value: 'cdf', label: 'CDF' },
    { value: 'scatter', label: 'Scatter Plot' },
    { value: 'heatmap', label: 'Heatmap' },
    { value: 'boxplot', label: 'Box Plot' },
    { value: 'violin', label: 'Violin Plot' },
    { value: 'line', label: 'Line Chart' },
    { value: 'donut', label: 'Donut Chart' },
    { value: 'stackedbar', label: 'Stacked Bar Chart' },
    { value: 'area', label: 'Area Chart' },
    { value: 'table', label: 'Data Table' },
  ],
  columns: [] as string[],
  aggregations: ['sum', 'count', 'mean', 'min', 'max'],

  reset() {
    this.selectedType = '';
    this.xColumn = '';
    this.yColumn = '';
    this.valueColumn = '';
    this.colorBy = '';
    this.stackColumn = '';
    this.categoryColumn = '';
    this.aggregation = 'sum';
  },

  canCreate(): boolean {
    if (!this.selectedType) return false;
    if (this.selectedType === 'table') return true;

    const typeRequirements: Record<string, string[]> = {
      bar: ['xColumn', 'yColumn'],
      histogram: ['xColumn'],
      cdf: ['xColumn'],
      scatter: ['xColumn', 'yColumn'],
      heatmap: ['xColumn', 'yColumn', 'valueColumn'],
      boxplot: ['xColumn', 'yColumn'],
      violin: ['xColumn', 'yColumn'],
      line: ['xColumn', 'yColumn'],
      donut: ['valueColumn', 'categoryColumn'],
      stackedbar: ['xColumn', 'yColumn', 'stackColumn'],
      area: ['xColumn', 'yColumn', 'stackColumn'],
    };

    const required = typeRequirements[this.selectedType] || [];
    return required.every(field => (this as any)[field]);
  },

  createChart() {
    if (!this.canCreate()) return;

    const config: any = {
      type: this.selectedType,
    };

    if (this.xColumn) config.xColumn = this.xColumn;
    if (this.yColumn) config.yColumn = this.yColumn;
    if (this.valueColumn) config.valueColumn = this.valueColumn;
    if (this.colorBy) config.colorBy = this.colorBy;
    if (this.stackColumn) config.stackColumn = this.stackColumn;
    if (this.categoryColumn) config.categoryColumn = this.categoryColumn;
    if (this.aggregation) config.aggregation = this.aggregation;

    chartState.addChart(config);
    this.reset();
  },
};

chartCreatorState.columns = state.dataSources[state.currentDataSourceKey].columns;

// Add this after chartCreatorState definition (around line 150)
(window as any).populateChartCreator = function(chartType: string, config: any) {
  chartCreatorState.selectedType = chartType;
  chartCreatorState.xColumn = config.xColumn || '';
  chartCreatorState.yColumn = config.yColumn || '';
  chartCreatorState.valueColumn = config.valueColumn || '';
  chartCreatorState.colorBy = config.colorBy || '';
  chartCreatorState.stackColumn = config.stackColumn || '';
  chartCreatorState.categoryColumn = config.categoryColumn || '';
  chartCreatorState.aggregation = config.aggregation || 'sum';

  // Open the right sidebar
  rightSidebarState.collapsed = false;

  m.redraw();
};

// Generic filterable list component
const FilterableList: m.Component<{
  title: string;
  placeholder: string;
  items: Array<{value: string, label: string}>;
  searchTerm: string;
  onSearch: (term: string) => void;
  onSelect: (value: string) => void;
}> = {
  view({attrs}) {
    const {title, placeholder, items, searchTerm, onSearch, onSelect} = attrs;
    const filteredItems = items.filter(
        (item) => item.label.toLowerCase().includes(searchTerm.toLowerCase()));

    return m('.side-nav-content',
      m('h3', title),
      m('input[type=text]', {
        placeholder,
        oninput: (e: Event) => onSearch((e.target as HTMLInputElement).value),
        value: searchTerm,
      }),
      m('ul',
        filteredItems.map(
            (item) => m(
                'li',
                m('button',
                  {
                    onclick: () => onSelect(item.value),
                  },
                  item.label),
                ),
            ),
        ),
    );
  }
};

const SchemaSidebar: m.Component = {
  view: () => {
    return m('.side-nav.left', {
      class: leftSidebarState.collapsed ? 'collapsed' : '',
    }, [
      !leftSidebarState.collapsed && m(FilterableList, {
        title: 'Select Schema',
        placeholder: 'Filter schemas...',
        items: Object.keys(state.dataSources).map(k => ({value: k, label: k})),
        searchTerm: chartCreatorState.schemaSearchTerm,
        onSearch: (term: string) => chartCreatorState.schemaSearchTerm = term,
        onSelect: (key: string) => {
          switchDataSource(key);
          leftSidebarState.toggle();
        },
      }),
    ]);
  },
};

// Autocomplete component
const AutocompleteInput: m.Component<{
  label: string;
  value: string;
  options: Array<{value: string; label: string}> | string[];
  onchange: (value: string) => void;
  placeholder?: string;
}> = {
  view: ({attrs}) => {
    const options = attrs.options.map(opt =>
      typeof opt === 'string' ? {value: opt, label: opt} : opt
    );

    return m('.autocomplete-field', [
      m('label.autocomplete-label', attrs.label),
      m('select.autocomplete-input', {
        value: attrs.value,
        onchange: (e: Event) => attrs.onchange((e.target as HTMLSelectElement).value),
      }, [
        m('option', {value: ''}, attrs.placeholder || 'Select...'),
        options.map(opt =>
          m('option', {value: opt.value}, opt.label)
        ),
      ]),
    ]);
  },
};

// Chart creator sidebar
const ChartCreatorSidebar: m.Component = {
  view: () => {
    const state = chartCreatorState;
    const showXColumn = ['bar', 'scatter', 'heatmap', 'boxplot', 'violin', 'line', 'stackedbar', 'area'].includes(state.selectedType);
    const showYColumn = ['bar', 'scatter', 'heatmap', 'boxplot', 'violin', 'line', 'stackedbar', 'area'].includes(state.selectedType);
    const showValueColumn = ['heatmap', 'donut'].includes(state.selectedType);
    const showCategoryColumn = ['donut'].includes(state.selectedType);
    const showStackColumn = ['stackedbar', 'area'].includes(state.selectedType);
    const showColorBy = ['scatter', 'cdf', 'line'].includes(state.selectedType);
    const showAggregation = ['bar', 'heatmap', 'stackedbar', 'area', 'line', 'donut'].includes(state.selectedType);
    const showColumnName = ['histogram', 'cdf'].includes(state.selectedType);

    return m('.side-nav.right', {
      class: rightSidebarState.collapsed ? 'collapsed' : '',
    }, [
      !rightSidebarState.collapsed && m('.sidebar-content', [
        !state.selectedType ?
        m(FilterableList, {
          title: 'Add Chart',
          placeholder: 'Filter charts...',
          items: state.chartTypes,
          searchTerm: state.chartTypeSearchTerm,
          onSearch: (term: string) => state.chartTypeSearchTerm = term,
          onSelect: (type: string) => {
            state.selectedType = type;
            m.redraw();
          },
        }) :
        m('.chart-options', [
          m('button.back-btn', {
            onclick: () => {
              state.selectedType = '';
              m.redraw();
            }
          }, '← Back to Charts'),
          m('h4', `Configure ${state.chartTypes.find(c => c.value === state.selectedType)?.label}`),

          showColumnName && m(AutocompleteInput, {
            label: 'Column',
            value: state.xColumn,
            options: state.columns,
            onchange: (val) => { state.xColumn = val; m.redraw(); },
          }),

        showXColumn && m(AutocompleteInput, {
          label: 'X Axis',
          value: state.xColumn,
          options: state.columns,
          onchange: (val) => { state.xColumn = val; m.redraw(); },
        }),

        showYColumn && m(AutocompleteInput, {
          label: 'Y Axis',
          value: state.yColumn,
          options: state.columns,
          onchange: (val) => { state.yColumn = val; m.redraw(); },
        }),

        showValueColumn && m(AutocompleteInput, {
          label: 'Value Column',
          value: state.valueColumn,
          options: state.columns,
          onchange: (val) => { state.valueColumn = val; m.redraw(); },
        }),

        showCategoryColumn && m(AutocompleteInput, {
          label: 'Category Column',
          value: state.categoryColumn,
          options: state.columns,
          onchange: (val) => { state.categoryColumn = val; m.redraw(); },
        }),

        showStackColumn && m(AutocompleteInput, {
          label: 'Stack Column',
          value: state.stackColumn,
          options: state.columns,
          onchange: (val) => { state.stackColumn = val; m.redraw(); },
        }),

        showColorBy && m(AutocompleteInput, {
          label: 'Color By (Optional)',
          value: state.colorBy,
          options: state.columns,
          onchange: (val) => { state.colorBy = val; m.redraw(); },
          placeholder: 'None',
        }),

        showAggregation && m(AutocompleteInput, {
          label: 'Aggregation',
          value: state.aggregation,
          options: state.aggregations,
          onchange: (val) => { state.aggregation = val; m.redraw(); },
        }),

        m('button.create-chart-btn', {
          disabled: !state.canCreate(),
          onclick: () => state.createChart(),
        }, 'Create Chart'),
        ]),
      ]),
    ]);
  },
};

// Dynamic chart renderer
function renderChart(config: ChartConfig, mdp: MemoryDataProvider, filterManager: FilterManager) {
  const commonProps = {
    dataProvider: mdp,
    filterManager: filterManager,
  };

  switch (config.type) {
    case 'bar':
      return m(D3BarChartComponent, {
        ...commonProps,
        xColumnName: config.xColumn,
        yColumnName: config.yColumn,
        aggregationFunction: config.aggregation,
      } as any);
    case 'histogram':
      return m(D3HistogramComponent, {
        ...commonProps,
        columnName: config.xColumn,
      } as any);
    case 'cdf':
      return m(D3CDFComponent, {
        ...commonProps,
        columnName: config.xColumn,
        colorBy: config.colorBy,
      } as any);
    case 'scatter':
      return m(D3ScatterChartComponent, {
        ...commonProps,
        xColumnName: config.xColumn,
        yColumnName: config.yColumn,
        colorBy: config.colorBy,
      } as any);
    case 'heatmap':
      return m(D3HeatmapChartComponent, {
        ...commonProps,
        xColumnName: config.xColumn,
        yColumnName: config.yColumn,
        valueColumnName: config.valueColumn,
        aggregationFunction: config.aggregation,
      } as any);
    case 'boxplot':
      return m(D3BoxplotChartComponent, {
        ...commonProps,
        xColumnName: config.xColumn,
        yColumnName: config.yColumn,
      } as any);
    case 'violin':
      return m(D3ViolinPlotChartComponent, {
        ...commonProps,
        xColumnName: config.xColumn,
        yColumnName: config.yColumn,
      } as any);
    case 'line':
      return m(D3LineChartComponent, {
        ...commonProps,
        xColumnName: config.xColumn,
        yColumnName: config.yColumn,
        colorBy: config.colorBy,
        aggregationFunction: config.aggregation,
      } as any);
    case 'donut':
      return m(D3DonutChartComponent, {
        ...commonProps,
        valueColumnName: config.valueColumn,
        categoryColumnName: config.categoryColumn,
        aggregationFunction: config.aggregation,
      } as any);
    case 'stackedbar':
      return m(D3StackedBarChartComponent, {
        ...commonProps,
        xColumnName: config.xColumn,
        yColumnName: config.yColumn,
        stackColumnName: config.stackColumn,
        aggregationFunction: config.aggregation,
      } as any);
    case 'area':
      return m(D3AreaChartComponent, {
        ...commonProps,
        xColumnName: config.xColumn,
        yColumnName: config.yColumn,
        stackColumnName: config.stackColumn,
        aggregationFunction: config.aggregation,
      } as any);
    case 'table':
      return m(DataTableComponent, {
        ...commonProps,
        dataProvider: new InMemoryDataSource(state.dataSources[state.currentDataSourceKey].data),
        columns: chartCreatorState.columns.map(c => ({name: c, title: c.charAt(0).toUpperCase() + c.slice(1)})),
      } as any);
    default:
      return m('div', 'Unknown chart type');
  }
}

const BigtracePage: m.Component = {
  view: () => m(
      '.bigtrace-page',
      {
        style: {
          display: 'grid',
          gridTemplateColumns: 'repeat(auto-fit, minmax(500px, 1fr))',
          gap: '1rem',
          overflow: 'auto',
          padding: '1rem',
        },
      },
      chartState.charts.map(config =>
        renderChart(config, mdp, filterManager)
      )
  ),
};

const BigTraceShell: m.Component = (() => {
  let optionsDropdownOpen = false;

  return {
    oncreate: (vnode) => {
      const menuElement = (vnode.dom as HTMLElement).querySelector('.options-menu');
      if (!menuElement) return;

      const clickHandler = (event: MouseEvent) => {
        if (optionsDropdownOpen && !menuElement.contains(event.target as Node)) {
          optionsDropdownOpen = false;
          m.redraw();
        }
      };

      document.addEventListener('click', clickHandler);

      (vnode.dom as any).__clickHandler = clickHandler;
    },
    onremove: (vnode) => {
      const clickHandler = (vnode.dom as any).__clickHandler;
      if (clickHandler) {
        document.removeEventListener('click', clickHandler);
      }
    },
    view: () => {
      return m(
          '.pf-ui-main',
        {
          style: {
            height: '100vh',
            display: 'flex',
            flexDirection: 'column',
          },
        },
        [
          m('.filter-behavior-toggle',
            {
              style:
                  'display: flex; justify-content: space-between; align-items: center; padding: 10px; background: #f5f5f5; border-bottom: 1px solid #ddd; flex-shrink: 0;',
            },
            [
              m('.header-left', {style: 'display: flex; align-items: center; gap: 8px;'},
                m('button.sidebar-toggle.left', {
                  class: !leftSidebarState.collapsed ? 'is-active' : '',
                  onclick: () => leftSidebarState.toggle(),
                }, m('div.sidebar-toggle-icon')),
                m('h1', {style: 'font-size: 16px; margin: 0;'}, `Perfetto | ${state.currentDataSourceKey}`),
                m('button.plus-btn', {
                  style: 'font-size: 20px; line-height: 1; padding: 0 5px;',
                  onclick: () => {
                    const fileInput = document.getElementById('schema-upload');
                    if (fileInput) {
                      fileInput.click();
                    }
                  },
                }, '+'),
                m('input[type=file]#schema-upload', {
                  style: 'display: none;',
                  accept: '.tsv',
                  onchange: (e: Event) => {
                    const target = e.target as HTMLInputElement;
                    if (target.files && target.files.length > 0) {
                      loadSchemaFromFile(target.files[0]);
                    }
                  },
                }),
              ),
              m('.header-right', {style: 'display: flex; align-items: center; gap: 8px;'},
                m('.options-menu', [
                  m('button.options-btn', {
                    onclick: () => {
                      optionsDropdownOpen = !optionsDropdownOpen;
                    },
                  }, m('.options-icon')),
                  optionsDropdownOpen && m('.options-dropdown', [
                    m('label',
                      {
                        style:
                            'display: flex; align-items: center; gap: 8px; font-family: sans-serif; font-size: 14px;',
                      },
                      [
                        m('input[type=checkbox]', {
                          checked: (window as any).GLOBAL_UPDATE_SOURCE_ON_FILTER,
                          onchange: (e: Event) => {
                            const target = e.target as HTMLInputElement;
                            (window as any).setUpdateSourceOnFilter(target.checked);
                          },
                        }),
                        m('span',
                          'Update source chart data on filter (when unchecked, source chart shows dimmed unselected regions)'),
                      ]),
                  ]),
                ]),
                m('button.sidebar-toggle.right', {
                  class: !rightSidebarState.collapsed ? 'is-active' : '',
                  onclick: () => rightSidebarState.toggle(),
                }, m('div.sidebar-toggle-icon')),
              ),
            ]),
          m('.main-container', {
            style: {
              display: 'flex',
              flex: '1',
              overflow: 'hidden',
            },
          }, [
            m(SchemaSidebar),
            m(BigtracePage),
            m(ChartCreatorSidebar),
          ]),
        ]);
    },
  };
})();


function onCssLoaded() {
  document.body.innerHTML = '';
  raf.mount(document.body, {
    view: () =>
      m(ThemeProvider, {theme: 'light'}, [
        m(OverlayContainer, {fillParent: true}, [
          m(BigTraceShell),
        ]),
      ]),
  });

  initLiveReload();
}

main();
