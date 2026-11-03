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

import m from 'mithril';
import {Icons} from '../../base/semantic_icons';
import {Time} from '../../base/time';
import {DurationWidget} from '../../components/widgets/duration';
import {Timestamp} from '../../components/widgets/timestamp';
import type {TrackEventDetailsPanel} from '../../public/details_panel';
import type {Trace} from '../../public/trace';
import type {DatasetSchema} from '../../trace_processor/dataset';
import type {Row} from '../../trace_processor/query_result';
import {sqlValueToReadableString} from '../../trace_processor/sql_utils';
import {Anchor} from '../../widgets/anchor';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {
  bindsToPeerTrackUri,
  boundByPeerTrackUri,
  providerBindsToPeerTrackUri,
  providerBoundByPeerTrackUri,
} from './aggregators';

export interface ProcessStateRow extends Row {
  readonly id: number;
  readonly ts: bigint;
  readonly dur: bigint;
}

/** Lists the columns of `schema` for one process state slice. */
export class ProcessStateDetailsPanel implements TrackEventDetailsPanel {
  constructor(
    private readonly trace: Trace,
    private readonly schema: DatasetSchema,
    private readonly row: ProcessStateRow,
    // Set on the overview row: links to the same slice on the owner track.
    private readonly ownerUri?: string,
  ) {}

  render() {
    const {trace, row, ownerUri} = this;
    return m(
      DetailsShell,
      {title: 'Slice'},
      m(
        GridLayout,
        m(
          Section,
          {title: 'Details'},
          m(Tree, [
            m(TreeNode, {left: 'ID', right: row.id}),
            m(TreeNode, {
              left: 'Start time',
              right: m(Timestamp, {trace, ts: Time.fromRaw(row.ts)}),
            }),
            m(TreeNode, {
              left: 'Duration',
              right: m(DurationWidget, {trace, dur: row.dur}),
            }),
            ...Object.keys(this.schema)
              .filter((key) => !['id', 'ts', 'dur'].includes(key))
              .map((key) => {
                const text = sqlValueToReadableString(row[key]);
                return m(TreeNode, {
                  left: key,
                  right:
                    key === 'process_name' && ownerUri !== undefined
                      ? m(
                          Anchor,
                          {
                            icon: Icons.UpdateSelection,
                            title: 'Show in process group',
                            onclick: () =>
                              trace.selection.selectTrackEvent(
                                ownerUri,
                                row.id,
                                {scrollToSelection: true},
                              ),
                          },
                          text,
                        )
                      : text,
                });
              }),
          ]),
        ),
      ),
    );
  }
}

export interface BindingSliceRow extends Row {
  readonly id: number;
  readonly ts: bigint;
  readonly dur: bigint;
  readonly binding_kind: string;
  readonly client_upid: number;
  readonly client_pid: number;
  readonly client_process_name: string | null;
  readonly server_upid: number;
  readonly server_pid: number;
  readonly server_process_name: string | null;
  readonly target: string;
  readonly intent_action: string | null;
  readonly bind_flags: string | null;
  readonly stability: string | null;
}

export class BindingSliceDetailsPanel implements TrackEventDetailsPanel {
  constructor(
    private readonly trace: Trace,
    private readonly row: BindingSliceRow,
    private readonly direction: 'bound_by' | 'binds_to',
    private readonly ownerUri?: string,
  ) {}

  render() {
    const {trace, row, direction, ownerUri} = this;
    const formatEndpoint = (name: string | null, pid: number): string =>
      name !== null ? `${name} (pid ${pid})` : `pid ${pid}`;

    const renderEndpoint = (
      text: string,
      targetUri: string,
      isSameDirection: boolean,
      peerTitle: string,
    ): m.Children => {
      if (isSameDirection && ownerUri === undefined) {
        return text;
      }
      const resolvedUri = isSameDirection ? ownerUri! : targetUri;
      if (trace.tracks.getTrack(resolvedUri) === undefined) {
        return text;
      }
      return m(
        Anchor,
        {
          icon: Icons.UpdateSelection,
          title: isSameDirection ? 'Show in process group' : peerTitle,
          onclick: () =>
            trace.selection.selectTrackEvent(resolvedUri, row.id, {
              scrollToSelection: true,
            }),
        },
        text,
      );
    };

    return m(
      DetailsShell,
      {title: 'Binding slice'},
      m(
        GridLayout,
        m(
          Section,
          {title: 'Details'},
          m(Tree, [
            m(TreeNode, {left: 'ID', right: row.id}),
            m(TreeNode, {
              left: 'Start time',
              right: m(Timestamp, {trace, ts: Time.fromRaw(row.ts)}),
            }),
            m(TreeNode, {
              left: 'Duration',
              right: m(DurationWidget, {trace, dur: row.dur}),
            }),
            m(TreeNode, {left: 'Kind', right: row.binding_kind}),
            m(TreeNode, {
              left: 'Client',
              right: renderEndpoint(
                formatEndpoint(row.client_process_name, row.client_pid),
                row.binding_kind === 'provider'
                  ? providerBindsToPeerTrackUri(
                      row.client_upid,
                      row.server_upid,
                    )
                  : bindsToPeerTrackUri(row.client_upid, row.server_upid),
                direction === 'binds_to',
                row.binding_kind === 'provider'
                  ? "Go to client's Provider binds to slice"
                  : "Go to client's Binds to slice",
              ),
            }),
            m(TreeNode, {
              left: 'Server',
              right: renderEndpoint(
                formatEndpoint(row.server_process_name, row.server_pid),
                row.binding_kind === 'provider'
                  ? providerBoundByPeerTrackUri(
                      row.server_upid,
                      row.client_upid,
                    )
                  : boundByPeerTrackUri(row.server_upid, row.client_upid),
                direction === 'bound_by',
                row.binding_kind === 'provider'
                  ? "Go to server's Provider bound by slice"
                  : "Go to server's Bound by slice",
              ),
            }),
            m(TreeNode, {left: 'Target', right: row.target}),
            row.intent_action !== null &&
              row.intent_action !== '' &&
              m(TreeNode, {left: 'Intent action', right: row.intent_action}),
            row.bind_flags !== null &&
              row.bind_flags !== '' &&
              m(TreeNode, {left: 'Bind flags', right: row.bind_flags}),
            row.stability !== null &&
              row.stability !== '' &&
              m(TreeNode, {left: 'Stability', right: row.stability}),
          ]),
        ),
      ),
    );
  }
}
