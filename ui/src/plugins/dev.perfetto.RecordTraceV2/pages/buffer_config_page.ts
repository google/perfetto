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
import {RecordingManager} from '../recording_manager';
import {assetSrc} from '../../../base/assets';
import {Slider} from './widgets/slider';
import {RecordMode, TraceConfigBuilder} from '../config/trace_config_builder';
import {ConfigManager} from '../config/config_manager';
import {RecordSubpage} from '../config/config_interfaces';
import {RecordSessionSchema} from '../serialization_schema';
import {Toggle} from './widgets/toggle';

type RecMgrAttrs = {recMgr: RecordingManager};

export function bufferConfigPage(recMgr: RecordingManager): RecordSubpage {
  return {
    kind: 'SESSION_PAGE',
    id: 'config',
    icon: 'tune',
    title: 'Buffers and duration',
    subtitle: 'Buffer mode, size and duration',
    render() {
      return m(BufferConfigPage, {recMgr});
    },
    serialize(state: RecordSessionSchema) {
      const tc: TraceConfigBuilder = recMgr.recordConfig.traceConfig;
      state.mode = tc.mode;
      state.bufSizeKb = tc.defaultBuffer.sizeKb;
      state.durationMs = tc.durationMs;
      state.maxFileSizeMb = tc.maxFileSizeMb;
      state.fileWritePeriodMs = tc.fileWritePeriodMs;
      state.compression = tc.compression;
    },
    async deserialize(state: RecordSessionSchema) {
      const tc: TraceConfigBuilder = recMgr.recordConfig.traceConfig;
      tc.mode = state.mode;
      tc.defaultBuffer.sizeKb = state.bufSizeKb;
      tc.durationMs = state.durationMs;
      tc.maxFileSizeMb = state.maxFileSizeMb;
      tc.fileWritePeriodMs = state.fileWritePeriodMs;
      tc.compression = state.compression;
    },
  };
}

class BufferConfigPage implements m.ClassComponent<RecMgrAttrs> {
  private bufSize: Slider;
  private maxDuration: Slider;
  private maxFileSize: Slider;
  private flushPeriod: Slider;
  private compress?: Toggle;

  constructor({attrs}: m.CVnode<RecMgrAttrs>) {
    const traceCfg = attrs.recMgr.recordConfig.traceConfig;
    this.bufSize = new Slider({
      title: 'In-memory buffer size',
      icon: '360',
      values: [4, 8, 16, 32, 64, 128, 256, 512],
      default: traceCfg.defaultBuffer.sizeKb / 1024,
      unit: 'MB',
      onChange: (v: number) => (traceCfg.defaultBuffer.sizeKb = v * 1024),
    });
    this.maxDuration = new Slider({
      title: 'Max duration',
      icon: 'timer',
      values: [S(10), S(15), S(30), S(60), M(5), M(30), H(1), H(6), H(12)],
      default: traceCfg.durationMs,
      isTime: true,
      unit: 'h:m:s',
      onChange: (value: number) => (traceCfg.durationMs = value),
    });
    this.maxFileSize = new Slider({
      title: 'Max file size',
      icon: 'save',
      values: [5, 25, 50, 100, 500, 1000, 1000 * 5, 1000 * 10],
      default: traceCfg.maxFileSizeMb,
      unit: 'MB',
      onChange: (value: number) => (traceCfg.maxFileSizeMb = value),
    });
    this.flushPeriod = new Slider({
      title: 'Flush on disk every',
      icon: 'av_timer',
      values: [100, 250, 500, 1000, 2500, 5000],
      default: traceCfg.fileWritePeriodMs,
      unit: 'ms',
      onChange: (value: number) => (traceCfg.fileWritePeriodMs = value),
    });
    if (!attrs.recMgr.currentTarget?.emitsCompressedtrace) {
      this.compress = new Toggle({
        title: 'Deflate (gzip) compression ',
        descr:
          'Generates smaller trace files at the cost of extra CPU cycles ' +
          'when stopping the trace. Compression happens only after the end of ' +
          'the trace and does not improve the ring-buffer efficiency.',
        default: traceCfg.compression,
        onChange: (enabled) => (traceCfg.compression = enabled),
      });
    }
  }

  view({attrs}: m.CVnode<RecMgrAttrs>) {
    const recCfg = attrs.recMgr.recordConfig;
    return [
      m('header', 'Recording mode'),
      m(
        '.record-mode',
        this.recButton(
          recCfg,
          'STOP_WHEN_FULL',
          'Stop when full',
          'rec_one_shot.png',
        ),
        this.recButton(
          recCfg,
          'RING_BUFFER',
          'Ring buffer',
          'rec_ring_buf.png',
        ),
        this.recButton(
          recCfg,
          'LONG_TRACE',
          'Long trace',
          'rec_long_trace.png',
        ),
      ),
      this.bufSize.render(),
      this.maxDuration.render(),
      recCfg.traceConfig.mode === 'LONG_TRACE' && this.maxFileSize.render(),
      recCfg.traceConfig.mode === 'LONG_TRACE' && this.flushPeriod.render(),
      this.compress?.render(),
    ];
  }

  recButton(
    recCfg: ConfigManager,
    mode: RecordMode,
    title: string,
    img: string,
  ) {
    const checkboxArgs = {
      checked: recCfg.traceConfig.mode === mode,
      onchange: (e: InputEvent) => {
        const checked = (e.target as HTMLInputElement).checked;
        if (!checked) return;
        recCfg.traceConfig.mode = mode;
        if (
          mode === 'LONG_TRACE' &&
          this.maxDuration.value === this.maxDuration.attrs.default
        ) {
          this.maxDuration.setValue(H(6));
        }
      },
    };
    return m(
      `label${recCfg.traceConfig.mode === mode ? '.selected' : ''}`,
      m(`input[type=radio][name=rec_mode]`, checkboxArgs),
      m(`img[src=${assetSrc(`assets/${img}`)}]`),
      m('span', title),
    );
  }
}

const S = (x: number) => x * 1000;
const M = (x: number) => x * 1000 * 60;
const H = (x: number) => x * 1000 * 60 * 60;
