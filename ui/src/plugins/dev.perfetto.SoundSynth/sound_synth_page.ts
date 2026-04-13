// Copyright (C) 2026 The Android Open Source Project
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

// Main SoundSynth page. Two-canvas layout:
//   - Top: rack canvas (instruments, trace sources, master)
//   - Bottom: instrument editor canvas (when an instrument is selected)
// Plus: left track browser, bottom transport bar.

import m from 'mithril';
import {Trace} from '../../public/trace';
import {NUM, NUM_NULL, STR, STR_NULL} from '../../trace_processor/query_result';
import protos from '../../protos';
import {
  PatchView,
  addTraceSource,
  buildRenderPatch,
  buildTestPatch,
  computePatchView,
  createEmptyPatch,
  importPresetAsInstrument,
  parsePatchUiState,
  writePatchUiState,
} from './patch_state';
import {TrackBrowser, ProcessInfo, TrackInfo} from './track_browser';
import {RackCanvas} from './rack_canvas';
import {InstrumentCanvas} from './instrument_canvas';
import {Transport} from './transport';
import {
  PresetLibrary, PresetEntry, loadPresetLibrary,
} from './preset_library';
import {PresetPicker} from './preset_picker';

interface SoundSynthPageAttrs {
  trace: Trace;
}

type PresetPickerTarget =
  | {kind: 'new_instrument'}
  | {kind: 'replace_instrument'; instrumentId: string};

export class SoundSynthPage implements m.ClassComponent<SoundSynthPageAttrs> {
  private trace: Trace | undefined;
  private state: protos.ISynthesizeAudioArgs = {};
  private processes: ProcessInfo[] = [];
  private loaded = false;
  private rendering = false;
  private wavData: ArrayBuffer | null = null;
  private autoPlayOnReady = false;
  private presetLibrary: PresetLibrary | null = null;
  private presetPickerTarget: PresetPickerTarget | null = null;
  private selectedInstrumentId: string | null = null;

  async oncreate(vnode: m.VnodeDOM<SoundSynthPageAttrs>) {
    this.trace = vnode.attrs.trace;
    await Promise.all([
      this.loadTrackMetadata(),
      this.loadPresets(),
    ]);
    this.state = createEmptyPatch();
    this.loaded = true;
    m.redraw();
  }

  private async loadPresets() {
    try {
      this.presetLibrary = await loadPresetLibrary();
    } catch (e) {
      console.error('Failed to load preset library:', e);
    }
  }

  view(vnode: m.Vnode<SoundSynthPageAttrs>) {
    this.trace = vnode.attrs.trace;
    if (!this.loaded) {
      return m('.sound-synth-page',
        {style: {padding: '20px'}},
        'Loading track data and preset library...');
    }

    const patch = this.state.patch!;
    const patchUi = parsePatchUiState(patch.uiStateJson);
    // Prefer local state but fall back to persisted state.
    const editingId =
      this.selectedInstrumentId ?? patchUi.editingInstrumentId;
    const view = computePatchView(patch);
    const editingInst = editingId
      ? view.instruments.find((i) => i.instrumentId === editingId) ?? null
      : null;

    const bindingCounts = this.computeBindingCounts(view);

    // Outer wrapper is position: relative, so the inner (absolute) can
    // fill it regardless of the parent's height chain.
    return m('.sound-synth-page', {
      style: {
        position: 'relative',
        width: '100%',
        height: '100%',
        minHeight: '400px',  // Fallback when parent has no defined height.
        overflow: 'hidden',
        fontFamily: 'Roboto, sans-serif',
      },
    }, m('.sound-synth-inner', {
      style: {
        position: 'absolute',
        top: '0',
        left: '0',
        right: '0',
        bottom: '0',
        display: 'flex',
        flexDirection: 'column',
        minHeight: '0',
      },
    },
      // Header.
      this.renderHeader(),
      // Body: left sidebar + main split canvases.
      m('.sound-synth-main', {
        style: {
          display: 'flex',
          flex: '1',
          minHeight: '0',
          overflow: 'hidden',
        },
      },
        m(TrackBrowser, {
          processes: this.processes,
          bindingCounts,
          onTrackPick: (track) => this.onTrackPick(track),
        }),
        m('.canvas-stack', {
          style: {
            flex: '1',
            display: 'flex',
            flexDirection: 'column',
            minWidth: '0',
            minHeight: '0',
          },
        },
          // Rack toolbar.
          this.renderRackToolbar(patch, view),
          // Rack canvas (top). Uses position: relative so the inner
          // NodeGraph canvas can fill it via absolute positioning.
          m('.rack-canvas-wrapper', {
            style: {
              flex: editingInst ? '1 1 0' : '1.5 1 0',
              minHeight: '260px',
              position: 'relative',
              overflow: 'hidden',
              borderBottom: '2px solid #ccc',
            },
          },
            m('.rack-canvas-inner', {
              style: {
                position: 'absolute',
                top: '0', left: '0', right: '0', bottom: '0',
                display: 'flex',
              },
            },
              m(RackCanvas, {
                patch,
                view,
                selectedInstrumentId: editingId,
                onEditInstrument: (id) => {
                  this.selectedInstrumentId = id;
                  writePatchUiState(patch, {editingInstrumentId: id});
                },
                onTestInstrument: (id) => this.onTestInstrument(id, view),
                onChange: () => { /* mithril auto-redraws */ },
              }),
            ),
          ),
          // Instrument editor (bottom).
          m('.instrument-canvas-wrapper', {
            style: {
              flex: '1 1 0',
              minHeight: '260px',
              position: 'relative',
              overflow: 'hidden',
              background: editingInst ? 'white' : '#f5f5f5',
            },
          },
            m('.instrument-canvas-inner', {
              style: {
                position: 'absolute',
                top: '0', left: '0', right: '0', bottom: '0',
                display: 'flex',
                flexDirection: 'column',
              },
            },
              editingInst
                ? m(InstrumentCanvas, {
                    patch,
                    instrument: editingInst,
                    onTest: () => this.onTestInstrument(
                      editingInst.instrumentId, view),
                    onClose: () => {
                      this.selectedInstrumentId = null;
                      writePatchUiState(patch, {editingInstrumentId: null});
                    },
                    onChange: () => { /* mithril auto-redraws */ },
                  })
                : this.renderInstrumentEmptyState(),
            ),
          ),
        ),
      ),
      // Transport bar.
      m(Transport, {
        rendering: this.rendering,
        wavData: this.wavData,
        autoPlay: this.autoPlayOnReady,
        onRender: () => this.doRender(view),
        onPlaybackStarted: () => {
          this.autoPlayOnReady = false;
        },
      }),
      // Preset picker overlay.
      this.presetPickerTarget && this.presetLibrary
        ? m(PresetPicker, {
            library: this.presetLibrary,
            onPick: (entry) => this.onPresetPicked(entry),
            onClose: () => {
              this.presetPickerTarget = null;
              m.redraw();
            },
          })
        : null,
    ),  // end .sound-synth-inner
    );
  }

  private renderHeader(): m.Child {
    const total = this.presetLibrary?.all().length ?? 0;
    return m('.sound-synth-header', {
      style: {
        display: 'flex',
        alignItems: 'center',
        padding: '8px 14px',
        borderBottom: '1px solid #ddd',
        background: '#f8f8fa',
        gap: '10px',
      },
    },
      m('span', {
        style: {fontWeight: 'bold', fontSize: '16px'},
      }, 'Sound Synth'),
      m('span', {
        style: {fontSize: '11px', color: '#888'},
      }, `${total} presets loaded`),
      m('.spacer', {style: {flex: '1'}}),
      m('span', {style: {fontSize: '11px', color: '#888'}},
        'Rack (top) → macro wiring · Instrument (bottom) → synth editor'),
    );
  }

  private renderRackToolbar(
    patch: protos.ISynthPatch,
    _view: PatchView,
  ): m.Child {
    return m('.rack-toolbar', {
      style: {
        display: 'flex',
        alignItems: 'center',
        padding: '6px 12px',
        borderBottom: '1px solid #e0e0e0',
        gap: '8px',
        background: '#eef1f6',
        fontSize: '11px',
      },
    },
      m('span',
        {style: {color: '#555', fontWeight: 'bold'}},
        'RACK'),
      m('button', {
        style: {
          padding: '4px 14px',
          fontSize: '11px',
          cursor: 'pointer',
          borderRadius: '3px',
          border: '1px solid #888',
          background: '#3f51b5',
          color: 'white',
          fontWeight: 'bold',
        },
        onclick: () => {
          this.presetPickerTarget = {kind: 'new_instrument'};
        },
      }, '+ Preset'),
      m('button', {
        style: {
          padding: '4px 10px',
          fontSize: '11px',
          cursor: 'pointer',
          borderRadius: '3px',
          border: '1px solid #888',
          background: '#fff',
        },
        onclick: () => {
          // Quick-add an empty trace source at a random-ish location.
          addTraceSource(
            patch, '*', 'Trace Source',
            80 + Math.random() * 120, 80 + Math.random() * 160);
        },
      }, '+ Trace Source'),
      m('.spacer', {style: {flex: '1'}}),
      m('span',
        {style: {color: '#888', fontSize: '10px'}},
        'Click a track in the left panel to add it as a trace source · ' +
        'Click an instrument\u2019s Edit button to tweak it below'),
    );
  }

  private renderInstrumentEmptyState(): m.Child {
    return m('.instrument-empty', {
      style: {
        flex: '1',
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
        flexDirection: 'column',
        gap: '8px',
        color: '#888',
        fontSize: '12px',
      },
    },
      m('div', {style: {fontSize: '24px'}}, '\u266B'),
      m('div', 'Select an instrument on the rack above'),
      m('div', 'and click Edit to view its internal patch'),
    );
  }

  private computeBindingCounts(view: PatchView): Map<string, number> {
    // Show, per process, how many trace sources reference it.
    const counts = new Map<string, number>();
    for (const src of view.traceSources) {
      const glob = src.module.traceSliceSource?.trackNameGlob ?? '';
      for (const proc of this.processes) {
        if (proc.processName && glob.includes(proc.processName)) {
          counts.set(
            proc.processName,
            (counts.get(proc.processName) ?? 0) + 1,
          );
        }
      }
    }
    return counts;
  }

  private onTrackPick(track: TrackInfo) {
    const patch = this.state.patch!;
    const glob = track.trackName ?? '*';
    const label = track.threadName
      ? `${track.threadName}/${track.trackName ?? ''}`
      : track.trackName ?? 'Track';
    const x = 60 + Math.random() * 120;
    const y = 80 + Math.random() * 200;
    addTraceSource(patch, glob, label, x, y);
    m.redraw();
  }

  private onPresetPicked(entry: PresetEntry) {
    console.log('[SoundSynth] onPresetPicked:', entry.name,
      'target:', this.presetPickerTarget);
    const target = this.presetPickerTarget;
    if (!target) return;
    const patch = this.state.patch!;
    console.log('[SoundSynth] BEFORE import: modules=',
      patch.modules?.length,
      'wires=', patch.wires?.length);

    if (target.kind === 'new_instrument') {
      const x = 380 + Math.random() * 80;
      const y = 100 + Math.random() * 200;
      const instId = importPresetAsInstrument(
        patch, entry, entry.name, x, y);
      this.selectedInstrumentId = instId;
      writePatchUiState(patch, {editingInstrumentId: instId});
      console.log('[SoundSynth] AFTER import: modules=',
        patch.modules?.length,
        'wires=', patch.wires?.length,
        'selectedInstrumentId=', instId);
    }
    // TODO (Milestone 2): replace_instrument — deletes the current
    // instrument and imports fresh.

    this.presetPickerTarget = null;
    m.redraw();
  }

  private async loadTrackMetadata() {
    if (!this.trace) return;
    const engine = this.trace.engine;

    const result = await engine.query(`
      SELECT
        t.id AS trackId,
        t.name AS trackName,
        'slice' AS trackType,
        extract_arg(t.dimension_arg_set_id, 'upid') AS upid,
        p.name AS processName,
        extract_arg(t.dimension_arg_set_id, 'utid') AS utid,
        thread.name AS threadName
      FROM track t
      JOIN _slice_track_summary USING (id)
      LEFT JOIN process p ON extract_arg(t.dimension_arg_set_id, 'upid') = p.upid
      LEFT JOIN thread ON extract_arg(t.dimension_arg_set_id, 'utid') = thread.utid
      ORDER BY p.name, thread.name, t.name
    `);

    const tracksByProcess = new Map<number, ProcessInfo>();
    const it = result.iter({
      trackId: NUM,
      trackName: STR_NULL,
      trackType: STR,
      upid: NUM_NULL,
      processName: STR_NULL,
      utid: NUM_NULL,
      threadName: STR_NULL,
    });

    for (; it.valid(); it.next()) {
      const upid = it.upid ?? 0;
      const processName = it.processName ?? `<pid ${upid}>`;
      if (!tracksByProcess.has(upid)) {
        tracksByProcess.set(upid, {
          upid,
          processName,
          tracks: [],
        });
      }
      tracksByProcess.get(upid)!.tracks.push({
        trackId: it.trackId,
        trackName: it.trackName ?? '<unnamed>',
        trackType: it.trackType as 'slice' | 'counter',
        upid: it.upid,
        processName: it.processName,
        utid: it.utid,
        threadName: it.threadName,
      });
    }

    this.processes = Array.from(tracksByProcess.values()).sort((a, b) =>
      a.processName.localeCompare(b.processName),
    );
  }

  private async doRender(view: PatchView) {
    if (!this.trace || this.rendering) return;
    this.rendering = true;
    this.wavData = null;
    m.redraw();

    try {
      const renderPatch = buildRenderPatch(
        view,
        this.state.patch?.modules ?? [],
        this.state.patch?.wires ?? [],
      );

      const visible = this.trace.timeline.visibleWindow.toTimeSpan();
      const startTs = Number(visible.start);
      const MAX_TRACE_NS = 1_000_000_000;  // 1s trace = 48s audio
      const rawEndTs = Number(visible.end);
      const endTs = Math.min(rawEndTs, startTs + MAX_TRACE_NS);

      const result = await this.trace.engine.synthesizeAudio(
        renderPatch, startTs, endTs);
      if (result.error && result.error.length > 0) {
        console.error('Synth error:', result.error);
        return;
      }
      if (result.wavData && result.wavData.length > 0) {
        const buf = result.wavData;
        this.wavData = buf.buffer.slice(
          buf.byteOffset,
          buf.byteOffset + buf.byteLength,
        );
      }
    } catch (e) {
      console.error('Render failed:', e);
    } finally {
      this.rendering = false;
      m.redraw();
    }
  }

  private async onTestInstrument(instrumentId: string, view: PatchView) {
    if (!this.trace || this.rendering) return;
    const inst = view.instruments.find(
      (i) => i.instrumentId === instrumentId,
    );
    if (!inst) return;

    this.rendering = true;
    this.wavData = null;
    this.autoPlayOnReady = true;
    m.redraw();

    try {
      const testPatch = buildTestPatch(
        inst,
        this.state.patch?.modules ?? [],
        this.state.patch?.wires ?? [],
      );
      // Send with duration_seconds so TP uses the preset-preview path
      // and doesn't touch the trace. The proto field isn't exposed on
      // Engine.synthesizeAudio yet, so we use a 16-second-equivalent
      // trace window (16 / 48 ≈ 333 ms of trace time) instead.
      const startTs = 0;
      const endTs = Math.floor(1_000_000_000 / 48) * 16;  // 16 s of audio
      const result = await this.trace.engine.synthesizeAudio(
        testPatch, startTs, endTs);
      if (result.error && result.error.length > 0) {
        console.error('Test synth error:', result.error);
        return;
      }
      if (result.wavData && result.wavData.length > 0) {
        const buf = result.wavData;
        this.wavData = buf.buffer.slice(
          buf.byteOffset,
          buf.byteOffset + buf.byteLength,
        );
      }
    } catch (e) {
      console.error('Test failed:', e);
    } finally {
      this.rendering = false;
      m.redraw();
    }
  }
}
