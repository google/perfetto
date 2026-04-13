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

export interface TrackInfo {
  trackId: number;
  trackName: string;
  trackType: 'slice' | 'counter';
  upid: number | null;
  processName: string | null;
  utid: number | null;
  threadName: string | null;
}

export interface ProcessInfo {
  upid: number;
  processName: string;
  tracks: TrackInfo[];
}

interface TrackBrowserAttrs {
  processes: ProcessInfo[];
  bindingCounts: Map<string, number>;
  onTrackPick: (track: TrackInfo) => void;
}

export class TrackBrowser implements m.ClassComponent<TrackBrowserAttrs> {
  private expandedProcesses = new Set<number>();
  private searchQuery = '';

  view(vnode: m.Vnode<TrackBrowserAttrs>) {
    const {processes, bindingCounts, onTrackPick} = vnode.attrs;
    const query = this.searchQuery.toLowerCase();
    const filtered = query
      ? processes.filter(
          (p) =>
            p.processName.toLowerCase().includes(query) ||
            p.tracks.some(
              (t) =>
                t.trackName.toLowerCase().includes(query) ||
                (t.threadName ?? '').toLowerCase().includes(query),
            ),
        )
      : processes;

    return m(
      '.track-browser',
      {
        style: {
          width: '260px',
          minWidth: '200px',
          borderRight: '1px solid #ddd',
          overflowY: 'auto',
          fontSize: '12px',
          background: '#fafafa',
        },
      },
      m(
        '.track-browser-header',
        {
          style: {
            padding: '8px',
            borderBottom: '1px solid #e0e0e0',
          },
        },
        m('div',
          {style: {fontSize: '11px', color: '#666', marginBottom: '4px'}},
          'Trace Tracks (click to add)',
        ),
        m('input[type=text]', {
          placeholder: 'Search...',
          style: {width: '100%', padding: '4px 6px', boxSizing: 'border-box'},
          value: this.searchQuery,
          oninput: (e: InputEvent) => {
            this.searchQuery = (e.target as HTMLInputElement).value;
          },
        }),
      ),
      m(
        '.track-tree',
        {style: {padding: '4px 8px'}},
        filtered.map((proc) =>
          this.renderProcess(proc, bindingCounts, onTrackPick),
        ),
      ),
    );
  }

  private renderProcess(
    proc: ProcessInfo,
    bindingCounts: Map<string, number>,
    onTrackPick: (track: TrackInfo) => void,
  ) {
    const expanded = this.expandedProcesses.has(proc.upid);
    const count = bindingCounts.get(proc.processName) ?? 0;
    const dimmed = count === 0;

    return m(
      '.process-group',
      {key: proc.upid},
      m(
        '.process-header',
        {
          style: {
            display: 'flex',
            alignItems: 'center',
            cursor: 'pointer',
            padding: '3px 0',
            opacity: dimmed ? '0.5' : '1',
            gap: '4px',
          },
          onclick: () => {
            if (expanded) {
              this.expandedProcesses.delete(proc.upid);
            } else {
              this.expandedProcesses.add(proc.upid);
            }
          },
        },
        m('span',
          {style: {fontSize: '10px', width: '12px'}},
          expanded ? '\u25BC' : '\u25B6',
        ),
        m(
          'span',
          {
            style: {
              flex: '1',
              overflow: 'hidden',
              textOverflow: 'ellipsis',
              whiteSpace: 'nowrap',
              fontWeight: count > 0 ? 'bold' : 'normal',
            },
          },
          proc.processName,
        ),
        count > 0
          ? m('span.badge', {
              style: {
                background: '#4285f4',
                color: 'white',
                borderRadius: '8px',
                padding: '0 6px',
                fontSize: '10px',
                minWidth: '16px',
                textAlign: 'center',
              },
            }, `${count}`)
          : null,
        m('span',
          {style: {color: '#999', fontSize: '10px'}},
          `${proc.tracks.length}`,
        ),
      ),
      expanded
        ? m(
            '.track-list',
            {style: {paddingLeft: '16px'}},
            proc.tracks.map((track) =>
              m(
                '.track-item',
                {
                  key: track.trackId,
                  style: {
                    padding: '2px 4px',
                    overflow: 'hidden',
                    textOverflow: 'ellipsis',
                    whiteSpace: 'nowrap',
                    color: '#555',
                    cursor: 'pointer',
                    borderRadius: '2px',
                  },
                  onmouseenter: (e: MouseEvent) => {
                    (e.target as HTMLElement).style.background = '#e8eaf6';
                  },
                  onmouseleave: (e: MouseEvent) => {
                    (e.target as HTMLElement).style.background = 'transparent';
                  },
                  onclick: () => onTrackPick(track),
                  title: 'Click to add as trace source',
                },
                track.threadName
                  ? m('span', {style: {color: '#888'}}, `${track.threadName}/`)
                  : null,
                track.trackName,
              ),
            ),
          )
        : null,
    );
  }
}
