// Copyright (C) 2025 The Android Open Source Project
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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import m from 'mithril';
import Chart, {ActiveElement, ChartEvent, TooltipItem} from 'chart.js/auto';
import {Arg} from '../../../components/sql_utils/args';
import {Button} from '../../../widgets/button';
import {Icons} from '../../../base/semantic_icons';

interface Component {
  name: string;
  sizeBytes: number;
  instanceCount?: number;
  detail?: object;
}

interface Category {
  label: string;
  sizeBytes: number;
  components: Array<Component>;
}

interface MemoryDashboardAttrs {
  data?: Arg[];
}

class MemoryDashboard implements m.ClassComponent<MemoryDashboardAttrs> {
  private totalMemory: number = 0;
  private currentCategory: Category | undefined;
  private chartInstance: Chart | null = null;
  private categories: Map<string, Category> = new Map();
  private currentImageDetail: Component | null = null;
  private url: string = '';

  oninit(vnode: m.Vnode<MemoryDashboardAttrs>) {
    this.totalMemory = 0;
    this.chartInstance = null;

    if (vnode.attrs.data !== undefined) {
      this.processData(vnode.attrs.data);
      this.currentCategory = this.categories.get('UI');
      this.currentImageDetail = null;
    }
  }

  onupdate(vnode: m.Vnode<MemoryDashboardAttrs>) {
    if (vnode.attrs.data !== undefined) {
      this.processData(vnode.attrs.data);
    }
  }

  onremove() {
    if (this.chartInstance !== null) {
      this.chartInstance.destroy();
    }
  }

  private processData(data: Arg[]) {
    const mainCategoriesKeys = ['backgroundThreadScriptingEngine', 'mainThreadScriptingEngine', 'lynxTasmElement'];
    const categories: Map<string, Category> = new Map([
      ['backgroundThreadScriptingEngine', {label: 'BTS Engine', sizeBytes: 0, components: []}],
      ['mainThreadScriptingEngine', {label: 'MTS Engine', sizeBytes: 0, components: []}],
      ['lynxTasmElement', {label: 'Lynx TASM Element', sizeBytes: 0, components: []}],
      ['UI', {label: 'UI Components', sizeBytes: 0, components: []}],
    ]);
    // Process Arg[] array
    for (const arg of data) {
      const flatKey = arg.flatKey;
      const displayValue = arg.displayValue;
      if (flatKey === 'legacy_event.passthrough_utid' || flatKey === 'debug.instance_id') continue;
      if (flatKey === 'debug.sizeBytes') {
        // displayValue is a number string for total memory
        this.totalMemory = parseInt(displayValue, 10) || 0;
        continue;
      }
      if (flatKey === 'debug.url') {
        this.url = displayValue;
        continue;
      }
      // remove debug. prefix
      const key = flatKey.slice(6);
      // For other keys, displayValue is a JSON string
      try {
        const item = JSON.parse(displayValue);

        if (typeof item === 'object' && item !== null && item.hasOwnProperty('sizeBytes') === true) {
          const size = parseInt(item.sizeBytes, 10) || 0;
          let category: Category | undefined;
          if (mainCategoriesKeys.includes(key)) {
            category = categories.get(key);
          } else {
            category = categories.get('UI');
          }
          if (category) {
            category.sizeBytes += size;
            category.components.push({name: key, sizeBytes: size, ...item});
          }
        }
      } catch (e) {
        // Skip invalid JSON
        continue;
      }
    }

    this.categories = categories;
  }

  private formatBytes(bytes: number, decimals: number = 2): string {
    if (bytes === 0) return '0 Bytes';
    const k = 1024;
    const dm = decimals < 0 ? 0 : decimals;
    const sizes = ['Bytes', 'KB', 'MB', 'GB', 'TB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return parseFloat((bytes / Math.pow(k, i)).toFixed(dm)) + ' ' + sizes[i];
  }

  private renderChart(): m.Vnode {
    if (this.categories.size === 0) {
      return m('div.chart-placeholder', {
        style: {
          width: '300px',
          height: '300px',
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'center',
          color: '#666',
          fontSize: '14px',
        },
      }, 'No Data');
    }

    const categoriesArray = Array.from(this.categories.values());
    const colors = ['#3e95cd', '#8e5ea2', '#3cba9f', '#e8c3b9', '#c45850'];

    const chartData = {
      labels: categoriesArray.map((c) => c.label),
      datasets: [{
        data: categoriesArray.map((c) => c.sizeBytes),
        backgroundColor: colors.slice(0, categoriesArray.length),
        borderWidth: 0,
        hoverBorderWidth: 2,
        hoverBorderColor: '#fff',
      }],
    };

    return m('canvas', {
      style: {
        width: '300px',
        height: '300px',
        maxWidth: '300px',
        maxHeight: '300px',
      },
      oncreate: (vnode: m.VnodeDOM) => {
        const canvas = vnode.dom as HTMLCanvasElement;
        const ctx = canvas.getContext('2d');
        if (ctx) {
          // Destroy previous chart instance
          if (this.chartInstance !== null) {
            this.chartInstance.destroy();
          }

          this.chartInstance = new Chart(ctx, {
            type: 'doughnut',
            data: chartData,
            options: {
              responsive: true,
              maintainAspectRatio: true,
              cutout: '70%',
              animation: {
                animateRotate: true,
                duration: 1000,
              },
              plugins: {
                legend: {display: false},
                tooltip: {
                  backgroundColor: 'rgba(0, 0, 0, 0.8)',
                  titleColor: '#fff',
                  bodyColor: '#fff',
                  borderColor: '#333',
                  borderWidth: 1,
                  callbacks: {
                    label: (tooltipItem: TooltipItem<'doughnut'>) => {
                      const label = tooltipItem.label ?? '';
                      const value = Number(tooltipItem.raw) || 0;
                      const percentage = this.totalMemory > 0 ? ((value / this.totalMemory) * 100).toFixed(2) : '0';
                      return `${label}: ${this.formatBytes(value)} (${percentage}%)`;
                    },
                  },
                },
              },
              onClick: (_: ChartEvent, elements: ActiveElement[]) => {
                if (elements.length > 0) {
                  const index = elements[0].index;
                  const category = categoriesArray[index];
                  this.currentCategory = category;
                  this.currentImageDetail = null;
                  m.redraw();
                }
              },
            },
          });
        }
      },
      onupdate: (_: m.VnodeDOM) => {
        if (this.chartInstance !== null) {
          // Update chart data
          this.chartInstance.data = chartData;
          this.chartInstance.update('none');
        }
      },
    });
  }

  private renderLegend(): m.Vnode[] {
    if (this.categories.size === 0) return [];

    const colors = ['#3e95cd', '#8e5ea2', '#3cba9f', '#e8c3b9', '#c45850'];

    return Array.from(this.categories.values()).map((category, index) => {
      const percentage = this.totalMemory > 0 ? ((category.sizeBytes / this.totalMemory) * 100).toFixed(2) : '0';
      const isSelected = this.currentCategory?.label === category.label;

      return m('.legend-item', {
        onclick: () => {
          this.currentImageDetail = null;
          this.currentCategory = category;
        },
        style: {
          display: 'flex',
          alignItems: 'center',
          cursor: 'pointer',
          padding: '8px',
          borderRadius: '4px',
          backgroundColor: isSelected ? 'rgba(0, 0, 0, 0.1)' : 'transparent',
          transition: 'background-color 0.2s ease',
        },
        onmouseover: (e: Event) => {
          if (!isSelected) {
            (e.currentTarget as HTMLElement).style.backgroundColor = 'rgba(0, 0, 0, 0.05)';
          }
        },
        onmouseout: (e: Event) => {
          if (!isSelected) {
            (e.currentTarget as HTMLElement).style.backgroundColor = 'transparent';
          }
        },
      }, [
        m('.legend-color', {
          style: {
            width: '12px',
            height: '12px',
            borderRadius: '2px',
            marginRight: '8px',
            backgroundColor: colors[index % colors.length],
            flexShrink: 0,
          },
        }),
        m('.legend-label', {
          style: {
            flex: 1,
            fontSize: '14px',
            // fontWeight: isSelected ? '500' : '400',
          },
        }, category.label),
        m('.legend-value', {
          style: {
            fontSize: '12px',
            opacity: '0.8',
            fontWeight: '400',
            whiteSpace: 'nowrap',
          },
        }, `${this.formatBytes(category.sizeBytes)} (${percentage}%)`),
      ]);
    });
  }

  private renderUITable(components: Category['components']): m.Vnode {
    const sortedComponents = [...components].sort((a, b) => b.sizeBytes - a.sizeBytes);

    return m('table', {
      style: {
        width: '100%',
        tableLayout: 'fixed',
      },
    }, [
      m('thead', [
        m('tr', [
          m('th', {
            style: {
              width: '40%',
            },
          }, 'Component'),
          m('th', {
            style: {
              width: '15%',
            },
          }, 'Count'),
          m('th', {
            style: {
              width: '25%',
            },
          }, 'MemorySize'),
          m('th', {
            style: {
              width: '20%',
            },
          }, 'Percentage'),
        ]),
      ]),
      m('tbody',
        sortedComponents.map((component) => {
          const percentage = this.totalMemory > 0 ? ((component.sizeBytes / this.totalMemory) * 100).toFixed(2) : 0;

          return m('tr.ui-component-item', {
            style: {cursor: 'pointer'},
            onclick: () => {
              if (component.name === 'image') {
                this.currentImageDetail = component;
                m.redraw();
              }
            },
          }, [
            m('td', component.name),
            m('td', component.instanceCount ?? 'N/A'),
            m('td', this.formatBytes(component.sizeBytes)),
            m('td', `${percentage}%`),
          ]);
        }),
      ),
    ]);
  }

  private renderGenericTable(detail: object): m.Vnode {
    return m('table', [
      m('thead', [
        m('tr', [
          m('th', 'Key'),
          m('th', 'Value'),
        ]),
      ]),
      m('tbody',
        Object.entries(detail).map(([key, value]) =>
          m('tr', [
            m('td', key),
            m('td', String(value)),
          ]),
        ),
      ),
    ]);
  }

  private renderImageDetails(image: Component) {
    if (!image.detail) {
      return [m('p', 'No image data')];
    }

    const images = Object.entries(image.detail).map(([url, size]) => ({
      url,
      size: parseInt(String(size), 10),
    })).sort((a, b) => b.size - a.size);

    return [
      m('.details-header', {
        style: {
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'space-between',
          marginBottom: '16px',
        },
      }, [
        m('h3', {
          style: {
            margin: '0',
            fontSize: '1.1em',
          },
        }, 'Image Component Details'),
        m(Button, {
          icon: Icons.GoBack,
          onclick: () => {
            this.currentImageDetail = null;
            this.currentCategory = this.categories.get('UI')!;
            m.redraw();
          },
          label: 'Back',
        }),
      ]),
      m('table', {
        style: {
          width: '100%',
          tableLayout: 'fixed',
        },
      }, [
        m('thead', [
          m('tr', [
            m('th', {
              style: {
                width: '50%',
              },
            }, 'URL'),
            m('th', {
              style: {
                width: '30%',
              },
            }, 'Memory Usage'),
            m('th', {
              style: {
                width: '20%',
              },
            }, '% of Total'),
          ]),
        ]),
        m('tbody',
          images.map((image) => {
            const percentage = this.totalMemory > 0 ? ((image.size / this.totalMemory) * 100).toFixed(2) : 0;
            return m('tr', [
              m('td', {
                style: {
                  maxWidth: '300px',
                  wordBreak: 'break-all',
                  overflow: 'hidden',
                  textOverflow: 'ellipsis',
                  whiteSpace: 'nowrap',
                },
                title: image.url, // Show full URL on hover
              }, image.url),
              m('td', this.formatBytes(image.size)),
              m('td', `${percentage}%`),
            ]);
          }),
        ),
      ]),
    ];
  }

  private renderDetails(): m.Children {
    // If there are image details to display, prioritize showing image details
    if (this.currentImageDetail) {
      return this.renderImageDetails(this.currentImageDetail);
    }

    if (!this.currentCategory) {
      return m('p', 'Please click on a module in the chart or legend to view detailed information.');
    }

    let content: m.Children;
    if (this.currentCategory.label === 'UI Components') {
      content = this.renderUITable(this.currentCategory.components);
    } else if (this.currentCategory.components.length > 0 && this.currentCategory.components[0].detail !== undefined ) {
      content = this.renderGenericTable(this.currentCategory.components[0].detail);
    } else {
      content = m('p', 'No detailed component information available for this module.');
    }

    return [
      m('.details-header', {
        style: {
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'space-between',
          marginBottom: '16px',
        },
      }, [
        m('h3', {
          style: {
            margin: '0',
            fontSize: '1.1em',
          },
        }, `${this.currentCategory.label} Details`),
      ]),
      content,
    ];
  }

  view(_vnode: m.Vnode<MemoryDashboardAttrs>): m.Children {
    return m('div', {
      style: {
        width: '100%',
        fontFamily: '"Roboto Condensed", sans-serif',
        fontSize: '14px',
      },
    }, [
      // URL display row
      this.url ? m('.url-display', {
        style: {
          marginBottom: '16px',
          padding: '8px 12px',
          backgroundColor: 'rgba(0, 0, 0, 0.02)',
          borderRadius: '4px',
          fontSize: '12px',
          color: '#666',
        },
      }, [
        m('span', {
          style: {
            fontWeight: '500',
            marginRight: '8px',
          },
        }, 'URL:'),
        m('span', {
          style: {
            overflow: 'hidden',
            textOverflow: 'ellipsis',
            whiteSpace: 'nowrap',
            display: 'inline-block',
            maxWidth: 'calc(100% - 40px)',
            verticalAlign: 'top',
          },
          title: this.url,
        }, this.url),
      ]) : null,
      // Main content with dashboard and details panel
      m('div', {
        style: {
          display: 'flex',
          gap: '16px',
          width: '100%',
        },
      }, [
      m('.dashboard', {
        style: {
          flex: '1',
          display: 'flex',
          flexDirection: 'column',
          minWidth: '250px',
        },
      }, [
        m('.chart-container', {
          style: {
            position: 'relative',
            width: '100%',
            maxWidth: '300px',
            margin: '0 auto 16px auto',
          },
        }, [
          this.renderChart(),
          this.totalMemory > 0 ? m('.total-memory', {
            style: {
              position: 'absolute',
              top: '50%',
              left: '50%',
              transform: 'translate(-50%, -50%)',
              textAlign: 'center',
            },
          }, [
            m('.value', {
              style: {
                fontSize: '1.5em',
                fontWeight: 'bold',
              },
            }, this.formatBytes(this.totalMemory)),
            m('.label', {
              style: {
                fontSize: '0.8em',
                opacity: '0.7',
              },
            }, 'Total Memory'),
          ]) : null,
        ]),
        m('.legend-container', {
          style: {
            display: 'flex',
            flexDirection: 'column',
            gap: '8px',
          },
        }, this.renderLegend()),
      ]),
      m('.details-panel', {
        style: {
          flex: '1',
          minWidth: '300px',
        },
      }, [
        m('.details-content', this.renderDetails()),
      ]),
    ]),
    ]);
  }
}

export default MemoryDashboard;
