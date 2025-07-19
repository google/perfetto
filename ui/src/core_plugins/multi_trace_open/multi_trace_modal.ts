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
import {showModal} from '../../widgets/modal';
import {Button} from '../../widgets/button';
import {TraceFileStream} from '../../core/trace_stream';
import {WasmEngineProxy} from '../../trace_processor/wasm_engine_proxy';
import {uuidv4} from '../../base/uuid';
import {AppImpl} from '../../core/app_impl';
import {Intent} from '../../widgets/common';

interface TraceFile {
  file: File;
  analyzed: boolean;
  syncGroup?: string;
}

export function showMultiTraceModal() {
  const traces: TraceFile[] = [];
  let analyzing = false;

  async function addTraces() {
    if (analyzing) return;
    
    const input = document.createElement('input');
    input.setAttribute('type', 'file');
    input.setAttribute('multiple', 'multiple');
    input.style.display = 'none';
    
    input.addEventListener('change', async () => {
      if (!input.files) return;
      
      const newFiles = [...input.files];
      if (newFiles.length === 0) return;
      
      analyzing = true;
      m.redraw();
      
      for (const file of newFiles) {
        const trace = {
          file,
          analyzed: false,
        };
        traces.push(trace);
        m.redraw();
        
        await analyzeTrace(trace);
      }
      
      analyzing = false;
      m.redraw();
    });
    
    input.click();
  }

  async function analyzeTrace(trace: TraceFile) {
    if (trace.analyzed) return;
    
    const engine = new WasmEngineProxy(uuidv4());
    
    AppImpl.instance.omnibox.showStatusMessage(
      `Analyzing ${trace.file.name}`,
      3000,
    );
    
    const stream = new TraceFileStream(trace.file);
    engine.resetTraceProcessor({
      tokenizeOnly: true,
      cropTrackEvents: false,
      ingestFtraceInRawTable: false,
      analyzeTraceProtoContent: false,
      ftraceDropUntilAllCpusValid: false,
    });
    
    for (;;) {
      const res = await stream.readChunk();
      await engine.parse(res.data);
      if (res.eof) {
        await engine.notifyEof();
        break;
      }
    }
    
    trace.analyzed = true;
    m.redraw();
    
    engine[Symbol.dispose]();
  }

  function openTraces() {
    if (traces.length === 0) return;
    
    const files = traces.map(t => t.file);
    AppImpl.instance.openTraceFromMultipleFiles(files);
  }

  function removeTrace(index: number) {
    traces.splice(index, index + 1);
    m.redraw();
  }

  function setSyncGroup(index: number, group: string) {
    traces[index].syncGroup = group;
    m.redraw();
  }

  const modalKey = 'multi-trace-modal';
  
  showModal({
    title: 'Open Multiple Traces',
    key: modalKey,
    buttons: [
      {
        text: 'Open Traces',
        primary: true,
        action: () => {
          openTraces();
          return true;
        },
        disabled: traces.length === 0 || analyzing,
      },
    ],
    content: () => {
      return m('.multi-trace-modal', [
        m('.multi-trace-controls', [
          m(Button, {
            label: analyzing ? 'Analyzing...' : 'Add Traces',
            onclick: addTraces,
            disabled: analyzing,
            intent: Intent.Primary,
          }),
        ]),
        
        analyzing && traces.length === 0 ?
          m('.multi-trace-analyzing', 'Adding traces...') :
        traces.length === 0 ? 
          m('.multi-trace-empty', 'No traces added yet. Click "Add Traces" to begin.') :
          [
            analyzing && m('.multi-trace-analyzing', 'Analyzing traces...'),
            m('.multi-trace-list', 
            traces.map((trace, index) => 
              m('.multi-trace-item', [
                m('.multi-trace-info', [
                  m('.multi-trace-name', trace.file.name),
                  m('.multi-trace-size', `${(trace.file.size / (1024 * 1024)).toFixed(2)} MB`),
                  m('.multi-trace-status' + (trace.analyzed ? '.analyzed' : ''), 
                    trace.analyzed ? 'Analyzed' : 'Not analyzed'),
                ]),
                m('.multi-trace-actions', [
                  m('select.multi-trace-sync', {
                    onchange: (e: Event) => {
                      const target = e.target as HTMLSelectElement;
                      setSyncGroup(index, target.value);
                    },
                    value: trace.syncGroup || '',
                  }, [
                    m('option', {value: ''}, 'No sync group'),
                    m('option', {value: 'group1'}, 'Sync group 1'),
                    m('option', {value: 'group2'}, 'Sync group 2'),
                    m('option', {value: 'group3'}, 'Sync group 3'),
                  ]),
                  m(Button, {
                    label: 'Remove',
                    onclick: () => removeTrace(index),
                    disabled: analyzing,
                    compact: true,
                  }),
                ]),
              ])
            )
            ),
          ],
          
        m('.multi-trace-info-panel', [
          m('h3', 'Clock Synchronization'),
          m('p', 'Add traces to the same sync group to synchronize their clocks during analysis. This helps align events from different traces on a common timeline.'),
          m('h3', 'Multi-machine Analysis'),
          m('p', 'Traces from different machines can be analyzed together by adding them to this view. This allows you to correlate events across multiple devices or systems.'),
        ]),
      ]);
    },
  });
}
