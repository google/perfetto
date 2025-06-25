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

import {List, ListItem, Size} from 'construct-ui';

import {Popup, PopupPosition} from '../../widgets/popup';
import {Button} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {Callout} from '../../widgets/callout';
import {Spinner} from '../../widgets/spinner';
import {
  SourceMapDecodePopup,
  sourceMapState,
} from '../../source_map/source_map_state';
import {getSourceFileInfo} from '../../source_map/get_sourcemap_info';
import {downloadData} from '../../base/download_utils';
import {raf} from '../../core/raf_scheduler';
import {convertTraceToJsonOnly} from '../../frontend/trace_converter';
import {AppImpl} from '../../core/app_impl';
import {Checkbox} from '../../widgets/checkbox';

interface TraceEvent {
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  args: Record<string, any>;
  cat: string;
  name: string;
  thread_name: string;
  ph: string;
  tid: number;
  pid: number;
  ts: number;
  dur?: number;
}

class DecodeFinishedComponent implements m.ClassComponent<{}> {
  private is_downloading: boolean = false;

  private downloadNewTrace(data: ArrayBufferLike) {
    this.is_downloading = true;
    const filename = 'source_map_decode.pftrace';
    downloadData(filename, data);
    this.is_downloading = false;
  }

  view(): m.Children {
    return m(
      '.open_new_trace_button_group',
      m(Button, {
        label: 'Download New Trace',
        icon: 'download',
        onclick: () => {
          if (sourceMapState.state.sourceMapDecodedTrace?.buffer) {
            this.downloadNewTrace(
              sourceMapState.state.sourceMapDecodedTrace?.buffer,
            );
          }
        },
        compact: true,
        active: true,
        disabled: this.is_downloading,
        style: {
          padding: '6px 18px',
          borderRadius: '5px',
          minWidth: '180px',
          color: '#fff',
          backgroundColor: '#1A73E8',
          border: 'none',
          boxShadow: '0 2px 6px rgba(30, 34, 90, 0.06)',
          transition: 'background 0.2s',
          marginTop: '4px',
        },
      }),
      this.is_downloading ? m(Spinner) : null,
    );
  }
}

interface UploadSourceMapProps {
  url: string;
  onchange: (e: Event) => void;
}

class UploadSourceMap implements m.ClassComponent<UploadSourceMapProps> {
  view({attrs}: m.Vnode<UploadSourceMapProps>): m.Children {
    return m('.upload-file-group', [
      m('label.upload-label', [
        m('input[type=file]', {
          style: {display: 'none'},
          onchange: attrs.onchange,
        }),
        m('.upload-text', `Click to upload the sourcemap for: ${attrs.url}`),
      ]),
    ]);
  }
}

interface SourceMapListItemProps extends UploadSourceMapProps {
  checked: boolean;
}
class SourceMapListItem implements m.ClassComponent<SourceMapListItemProps> {
  view({attrs}: m.Vnode<SourceMapListItemProps>): m.Children {
    return m('.source-map-list-item', [
      m(Checkbox, {
        checked: attrs.checked,
        disabled: true,
      }),
      m(UploadSourceMap, {
        url: attrs.url,
        onchange: attrs.onchange,
      }),
    ]);
  }
}

export class SourceMapDecodePopupImpl implements SourceMapDecodePopup {
  private async decodeSourceMap() {
    return new Promise<ArrayBuffer | null>(async (resolve) => {
      const file = await AppImpl.instance.trace?.getTraceFile();
      if (!file) {
        resolve(null);
        return;
      }
      convertTraceToJsonOnly(file, async (data: string) => {
        sourceMapState.edit((draft) => {
          draft.sourceMapDecodeInfo.push({
            type: 'success',
            message: `Waitting Covert to json finish`,
            state: 'decoding',
          });
        });
        const originalTrace = JSON.parse(data);
        const traceEvents = (originalTrace.traceEvents as TraceEvent[]) ?? [];
        for (let i = 0; i < traceEvents.length; i++) {
          const event = traceEvents[i];
          if (event.args['args'] !== undefined) {
            event.args = event.args['args'];
          }
          if (event.cat === 'jsprofile') {
            event.cat = 'jsprofile_decoded';
            const url = event.args['url'] ?? '';
            const lineNumber = (event.args['lineNumber'] ?? 0) + 1;
            const columnNumber = event.args['columnNumber'] ?? 0;
            if (url !== '' && lineNumber >= 0 && columnNumber > 0) {
              const source = await getSourceFileInfo(
                url,
                lineNumber,
                columnNumber,
              );
              if (source != null && source !== undefined) {
                event.args['originSource'] =
                  source.source + ':' + source.line + ':' + source.column;
                event.name = source.name ?? event.name;
                event.args['originLine'] = source.line;
                event.args['originColumn'] = source.column;
              }
            }
          }
        }
        const sourceFiles: Array<{file: string; content: string}> = [];
        sourceMapState.state.sourceMapConsumerByUrl.forEach((consumer) => {
          // @ts-ignore
          if (consumer !== undefined && consumer.sources !== undefined) {
            // @ts-ignore
            const files = (consumer.sources as Array<string>) ?? [];
            // @ts-ignore
            const contents = (consumer.sourcesContent as Array<string>) ?? [];
            for (let i = 0; i < files.length; i++) {
              const file = files[i];
              const content = contents[i];
              sourceFiles.push({
                file,
                content,
              });
            }
          }
        });
        originalTrace.sourceFiles = sourceFiles;
        const encoder = new TextEncoder();
        resolve(encoder.encode(JSON.stringify(originalTrace)).buffer);
      });
    });
  }

  private async sourceMapDecode() {
    const buffer = await this.decodeSourceMap();
    if (buffer) {
      sourceMapState.edit((draft) => {
        draft.sourceMapDecodedTrace = {
          buffer,
          region: '',
        };
      });
    }
    sourceMapState.edit((draft) => {
      draft.sourceMapDecodeInfo.push({
        type: 'success',
        message: 'Decode Finished',
        state: 'decoding',
      });
    });
    raf.scheduleFullRedraw();
  }

  private renderListItem(url: string) {
    const handleChange = (e: Event) => {
      const target = e.target as HTMLInputElement;
      const files = target.files;
      if (files && files.length > 0) {
        const file = files[0];
        const reader = new FileReader();
        reader.onload = function () {
          const str = reader.result as string;
          sourceMapState.edit((draft) => {
            draft.sourceMapDataByUrl[url] = {
              key: url,
              data: str ?? '{}',
            };
          });
          console.log(`Read file: ${file.name} success`);
          raf.scheduleFullRedraw();
        };
        reader.onerror = function (e) {
          console.error(`Read file: ${file.name} fail`, e);
        };
        reader.readAsText(file);
      } else {
        console.error('FileList is empty');
      }
    };
    return m(SourceMapListItem, {
      url,
      checked: sourceMapState.state.sourceMapDataByUrl[url] !== undefined,
      onchange: handleChange,
    });
  }

  public render() {
    if (!sourceMapState.state.hasJSProfileTrace) {
      return null;
    }
    const children: m.Vnode[] = [];
    if (sourceMapState.state.sourceMapDecodeState === 'decoding') {
      children.push(
        m(
          Callout,
          {
            icon: 'info',
            style: {
              width: '100%',
              borderStyle: 'none',
              minHeight: '20px',
              minWidth: '300px',
              backgroundColor: 'inherit',
              height: 'fit-content',
            },
          },
          m(
            'text',
            {
              style: {
                width: '100%',
                height: '100%',
              },
            },
            'Waitting Decode finish',
          ),
        ),
      );

      children.push(
        // @ts-ignore
        sourceMapState.state.sourceMapDecodeInfo.map((info) => {
          return m(
            Callout,
            {
              icon: 'info',
              style: {
                width: '100%',
                minWidth: '300px',
                minHeight: '20px',
                overflow: 'hidden',
                backgroundColor: 'inherit',
                height: 'fit-content',
                borderStyle: 'none',
              },
            },
            m(
              'text',
              {
                style: {
                  width: '100%',
                  height: '100%',
                  whiteSpace: 'nowrap',
                  overflowX: 'auto',
                },
              },
              `${info.message}`,
            ),
          );
        }),
      );

      if (sourceMapState.state.sourceMapDecodedTrace?.buffer) {
        children.push(m(DecodeFinishedComponent));
      }
    } else if (sourceMapState.state.sourceMapDecodeState === 'init') {
      const idSet: Set<string> = new Set();
      sourceMapState.state.sourceMapInfoByUrl.forEach((info) => {
        idSet.add(info.key);
      });
      children.push(
        m(
          List,
          {
            interactive: true,
            size: Size.SM,
          },
          Array.from(idSet).map((item) =>
            m(ListItem, {label: this.renderListItem(item)}),
          ),
        ),
      );
      children.push(
        // @ts-ignore
        m(Button, {
          label: 'Start Decode',
          icon: 'play_circle',
          onclick: () => {
            sourceMapState.edit((draft) => {
              draft.sourceMapDecodeState = 'decoding';
            });
            raf.scheduleFullRedraw();
            this.sourceMapDecode();
          },
          style: {
            padding: '5px',
            minWidth: '40%',
          },
          compact: true,
          active: true,
        }),
      );
    }
    let label = '';
    let icon = '';
    switch (sourceMapState.state.sourceMapDecodeState) {
      case 'init': {
        label = 'SourceMap Decode';
        icon = 'source_notes';
        break;
      }
      case 'decoding': {
        label = 'SourceMap Decode';
        icon = 'sync';
        break;
      }
      case 'uploading': {
        label = 'New Trace Uploading';
        icon = 'sync';
        break;
      }
      case 'uploaded':
      case 'opened': {
        label = 'New Trace Uploaded';
        icon = 'verified';
      }
    }
    return m(
      Popup,
      {
        trigger: m(Button, {
          label,
          icon,
          intent: Intent.Primary,
        }),
        position: PopupPosition.Bottom,
        closeOnEscape: false,
        closeOnOutsideClick: false,
        className: 'source-popup',
      },
      m('.source-popup-content', children),
    );
  }
}
