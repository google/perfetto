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
import {RecordSubpage} from '../config/config_interfaces';
import {RecordPluginSchema} from '../serialization_schema';
import {Button} from '../../../widgets/button';
import {TracingSession} from '../interfaces/tracing_session';
import {uuidv4} from '../../../base/uuid';

interface SnapshotEntry {
  timestamp: Date;
  sizeBytes: number;
  error?: string;
}

export function snapshotPage(recMgr: RecordingManager): RecordSubpage {
  return {
    kind: 'GLOBAL_PAGE',
    id: 'snapshots',
    icon: 'photo_camera',
    title: 'Live Snapshots',
    subtitle: 'Take snapshots of a running trace',
    render() {
      return m(SnapshotPage, {recMgr});
    },
    serialize(_state: RecordPluginSchema) {},
    deserialize(_state: RecordPluginSchema) {},
  };
}

interface SnapshotPageAttrs {
  recMgr: RecordingManager;
}

class SnapshotPage implements m.ClassComponent<SnapshotPageAttrs> {
  private sessionName: string = '';
  private session?: TracingSession;
  private isRecording = false;
  private snapshots: SnapshotEntry[] = [];
  private isTakingSnapshot = false;
  private error?: string;

  view({attrs}: m.CVnode<SnapshotPageAttrs>) {
    const recMgr = attrs.recMgr;
    const target = recMgr.currentTarget;
    const supportsClone = target?.cloneSession !== undefined;

    if (!target) {
      return m('.snapshot-page', m('.note', 'Please select a target first.'));
    }

    if (!supportsClone) {
      return m(
        '.snapshot-page',
        m(
          '.note',
          'The current target does not support live snapshots. ',
          'Try using a WebSocket connection to traced.',
        ),
      );
    }

    return m(
      '.snapshot-page',
      m('header', 'Live Trace Snapshots'),
      m(
        'p',
        'Start a recording session and take snapshots at any time. ',
        'Each snapshot captures the current trace buffer without stopping the recording.',
      ),

      // Session controls
      m(
        '.snapshot-controls',
        !this.isRecording
          ? m(Button, {
              label: 'Start Recording',
              icon: 'fiber_manual_record',
              onclick: () => this.startRecording(recMgr),
            })
          : [
              m(Button, {
                label: this.isTakingSnapshot ? 'Taking...' : 'Take Snapshot',
                icon: 'photo_camera',
                disabled: this.isTakingSnapshot,
                onclick: () => this.takeSnapshot(recMgr),
              }),
              m(Button, {
                label: 'Stop Recording',
                icon: 'stop',
                onclick: () => this.stopRecording(),
              }),
            ],
      ),

      // Status
      this.isRecording &&
        m(
          '.snapshot-status',
          m('span.recording-indicator', '● Recording'),
          m('span', ` Session: ${this.sessionName}`),
        ),

      // Error display
      this.error && m('.snapshot-error', this.error),

      // Snapshots list
      this.snapshots.length > 0 && [
        m('header', 'Snapshots'),
        m(
          '.snapshot-list',
          this.snapshots.map((snap, i) =>
            m(
              '.snapshot-entry',
              {key: i},
              m('span.snapshot-num', `#${i + 1}`),
              m(
                'span.snapshot-time',
                snap.timestamp.toLocaleTimeString(),
              ),
              snap.error
                ? m('span.snapshot-error', snap.error)
                : m(
                    'span.snapshot-size',
                    this.formatBytes(snap.sizeBytes),
                  ),
            ),
          ),
        ),
      ],
    );
  }

  private async startRecording(recMgr: RecordingManager) {
    const target = recMgr.currentTarget;
    if (!target) return;

    this.error = undefined;
    this.snapshots = [];
    this.sessionName = `snapshot-session-${uuidv4().substring(0, 8)}`;

    // Generate trace config with a unique session name
    const traceConfig = recMgr.genTraceConfig();
    traceConfig.uniqueSessionName = this.sessionName;
    // Use ring buffer mode for continuous recording
    traceConfig.durationMs = 0; // No timeout
    traceConfig.writeIntoFile = false;

    const result = await target.startTracing(traceConfig);
    if (!result.ok) {
      this.error = `Failed to start recording: ${result.error}`;
      m.redraw();
      return;
    }

    this.session = result.value;
    this.isRecording = true;

    // Listen for session state changes
    this.session.onSessionUpdate.addListener(() => {
      if (this.session?.state === 'ERRORED') {
        this.error = 'Recording session errored';
        this.isRecording = false;
      }
      m.redraw();
    });

    m.redraw();
  }

  private async takeSnapshot(recMgr: RecordingManager) {
    const target = recMgr.currentTarget;
    if (!target?.cloneSession || !this.sessionName) return;

    this.isTakingSnapshot = true;
    this.error = undefined;
    m.redraw();

    const result = await target.cloneSession(this.sessionName);

    const entry: SnapshotEntry = {
      timestamp: new Date(),
      sizeBytes: 0,
    };

    if (result.ok) {
      entry.sizeBytes = result.value.length;
    } else {
      entry.error = result.error;
    }

    this.snapshots.push(entry);
    this.isTakingSnapshot = false;
    m.redraw();
  }

  private stopRecording() {
    this.session?.cancel();
    this.session = undefined;
    this.isRecording = false;
    m.redraw();
  }

  private formatBytes(bytes: number): string {
    if (bytes === 0) return '0 B';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return `${(bytes / Math.pow(k, i)).toFixed(1)} ${sizes[i]}`;
  }
}
