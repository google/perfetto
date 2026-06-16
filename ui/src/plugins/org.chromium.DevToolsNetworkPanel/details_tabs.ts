// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under_the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import m from 'mithril';
import type {Row} from '../../trace_processor/query_result';
import type {Trace} from '../../public/trace';
import {Button} from '../../widgets/button';
import {TextInput} from '../../widgets/text_input';
import {Icons} from '../../base/semantic_icons';

export interface NetworkRequest extends Row {
  readonly id: number;
  readonly ts: bigint;
  readonly dur: bigint;
  readonly url: string;
  readonly name: string;
  readonly method: string;
  readonly statusCode: number;
  readonly priority: string;
  readonly mimeType: string | null;
  readonly protocol: string | null;
  readonly encodedDataLength: number | null;
  readonly decodedBodyLength: number | null;
  readonly startTimeMs: number;
  readonly durationMs: number;
  readonly totalDurMs: number;
  readonly servedFromMemoryCache: number;
  readonly servedFromDiskCache: number;
  readonly queueingTimeMs: number | null;
  readonly sendDurationMs: number | null;
  readonly waitingTimeMs: number | null;
  readonly downloadDurationMs: number | null;
  readonly headers: string | null;
  readonly initiator: string | null;
  readonly timing: string | null;
  readonly domain: string;
  readonly timeline: string;
  readonly transferred: number;
  readonly transferredStr: string;
}

export interface DetailsTabAttrs {
  readonly request: NetworkRequest;
  readonly trace: Trace;
}

function formatDuration(durNs: bigint): string {
  if (durNs === 0n) return 'Instant';
  return (Number(durNs) / 1e6).toFixed(1) + ' ms';
}

export function formatSize(bytes: unknown, digits = 1): string {
  if (bytes === null || bytes === undefined) return '';
  const b = Number(bytes);
  if (isNaN(b)) return '';
  if (b === 0) return '0 B';
  if (b >= 1024 * 1024) {
    return (b / (1024 * 1024)).toFixed(digits) + ' MiB';
  }
  return (b / 1024).toFixed(digits) + ' KiB';
}

export function parseRawHeaders(str: string | null): {
  request: [string, string][];
  response: [string, string][];
} {
  const request: [string, string][] = [];
  const response: [string, string][] = [];
  if (!str) return {request, response};

  const fragments = str.split('|||');
  for (const frag of fragments) {
    try {
      const parsed = JSON.parse(frag);

      // Explicitly named request headers
      const explicitReq =
        parsed?.debug?.params?.request_headers ??
        parsed?.debug?.data?.requestHeaders ??
        parsed?.data?.requestHeaders ??
        parsed?.requestHeaders ??
        parsed?.request_headers;
      if (explicitReq) extractPairs(explicitReq, request);

      // Explicitly named response headers
      const explicitResp =
        parsed?.debug?.params?.response_headers ??
        parsed?.debug?.data?.responseHeaders ??
        parsed?.data?.responseHeaders ??
        parsed?.responseHeaders ??
        parsed?.response_headers;
      if (explicitResp) extractPairs(explicitResp, response);

      // Generic "headers" dictionary/array
      const generic =
        parsed?.debug?.params?.headers ??
        parsed?.debug?.data?.headers ??
        parsed?.data?.headers ??
        parsed?.headers;
      if (generic) {
        let isResponse = false;
        if (Array.isArray(generic) && generic.length > 0 && typeof generic[0] === 'string') {
          if (generic[0].trim().toUpperCase().startsWith('HTTP/')) {
            isResponse = true;
          }
        }
        if (isResponse) {
          extractPairs(generic, response);
        } else {
          extractPairs(generic, request);
        }
      }
    } catch {
      // Continue
    }
  }
  return {request, response};
}

function extractPairs(obj: unknown, out: [string, string][]): void {
  if (!obj) return;

  if (Array.isArray(obj)) {
    for (let idx = 0; idx < obj.length; idx++) {
      const item = obj[idx];
      if (typeof item === 'string') {
        const colon = item.indexOf(':');
        if (colon >= 0) {
          out.push([item.slice(0, colon).trim(), item.slice(colon + 1).trim()]);
        } else {
          out.push([`header[${idx}]`, item]);
        }
      } else if (item !== null && typeof item === 'object') {
        const name = (item as any).name;
        const value = (item as any).value;
        if (typeof name === 'string' && value !== undefined) {
          out.push([name, String(value)]);
        } else {
          out.push([`header[${idx}]`, JSON.stringify(item)]);
        }
      }
    }
  } else if (typeof obj === 'object' && obj !== null) {
    for (const [k, v] of Object.entries(obj)) {
      if (k === 'headers' && Array.isArray(v)) {
        extractPairs(v, out);
      } else if (typeof v === 'string') {
        let key = k;
        let val = v;
        if (k.startsWith('headers[')) {
          const colon = v.indexOf(':');
          if (colon >= 0) {
            key = v.slice(0, colon).trim();
            val = v.slice(colon + 1).trim();
          }
        }
        out.push([key, val]);
      } else if (v !== null && typeof v === 'object') {
        const name = (v as any).name;
        const value = (v as any).value;
        if (typeof name === 'string' && value !== undefined) {
          out.push([name, String(value)]);
        } else {
          extractPairs(v, out);
        }
      }
    }
  }
}

export function renderMetaTable(entries: [string, string][]): m.Vnode {
  return m(
    'table.pf-network-meta-table',
    entries.map(([key, val]) =>
      m(
        'tr',
        m('td.pf-network-meta-table__key', key),
        m('td.pf-network-meta-table__val', val),
      ),
    ),
  );
}

export function highlightSliceAndZoom(trace: Trace, request: NetworkRequest): void {
  trace.selection.selectSqlEvent('slice', request.id, {
    switchToCurrentSelectionTab: false,
    scrollToSelection: true,
  });
}

export function formatJsonPreview(str: string): string {
  try {
    const parsed = JSON.parse(str);
    return JSON.stringify(parsed, null, 2);
  } catch {
    return str;
  }
}

export class HeadersTab implements m.ClassComponent<DetailsTabAttrs> {
  private responseSearchQuery = '';
  private requestSearchQuery = '';

  view({attrs}: m.CVnode<DetailsTabAttrs>): m.Children {
    const {request, trace} = attrs;
    let cacheVal = 'No (network)';
    if (request.servedFromMemoryCache) {
      cacheVal = 'Yes (memory cache)';
    } else if (request.servedFromDiskCache) {
      cacheVal = 'Yes (disk cache)';
    }

    const generalEntries: [string, string][] = [
      ['Request URL', request.url],
      ['Request Method', request.method],
      ['Status Code', String(request.statusCode)],
      ['Duration', formatDuration(request.dur)],
      ['Priority', request.priority],
      ['Protocol', request.protocol ?? 'h2'],
      ['Type', request.mimeType ?? 'document'],
      ['Served From Cache', cacheVal],
    ];

    if (request.encodedDataLength !== null) {
      generalEntries.push([
        'Compressed Size',
        formatSize(request.encodedDataLength, 2),
      ]);
    }
    if (request.decodedBodyLength !== null) {
      generalEntries.push(['Decoded Size', formatSize(request.decodedBodyLength, 2)]);
    }

    const {request: requestEntries, response: responseEntries} =
      parseRawHeaders(request.headers);

    const respQuery = this.responseSearchQuery.trim().toLowerCase();
    const filteredResponse = respQuery
      ? responseEntries.filter(
          ([key, val]) =>
            key.toLowerCase().includes(respQuery) ||
            val.toLowerCase().includes(respQuery),
        )
      : responseEntries;

    const reqQuery = this.requestSearchQuery.trim().toLowerCase();
    const filteredRequest = reqQuery
      ? requestEntries.filter(
          ([key, val]) =>
            key.toLowerCase().includes(reqQuery) ||
            val.toLowerCase().includes(reqQuery),
        )
      : requestEntries;

    return m(
      '.pf-network-details__content',
      m(
        'div',
        {
          style: {
            display: 'flex',
            justifyContent: 'space-between',
            alignItems: 'center',
            marginBottom: '12px',
          },
        },
        m('h3', {style: {margin: 0}}, 'General'),
        m(Button, {
          label: 'Highlight Slice in Track View',
          icon: Icons.UpdateSelection,
          compact: true,
          tooltip:
            'Selects the network slice and sets the active timeline viewport/time range selection',
          onclick: () => {
            highlightSliceAndZoom(trace, request);
          },
        }),
      ),
      renderMetaTable(generalEntries),
      m(
        'div',
        {
          style: {
            display: 'flex',
            justifyContent: 'space-between',
            alignItems: 'center',
            marginTop: '20px',
            marginBottom: '12px',
            borderBottom: '1px solid var(--pf-color-border)',
            paddingBottom: '6px',
          },
        },
        m(
          'h3',
          {style: {margin: 0, borderBottom: 'none', paddingBottom: 0}},
          'Response Headers',
        ),
        m(TextInput, {
          leftIcon: Icons.Search,
          placeholder: 'Filter response...',
          value: this.responseSearchQuery,
          onInput: (val: string) => {
            this.responseSearchQuery = val;
          },
        }),
      ),
      filteredResponse.length > 0
        ? renderMetaTable(filteredResponse)
        : m(
            'p',
            responseEntries.length > 0
              ? 'No response headers match your search filter.'
              : 'Response headers not recorded in this trace event (enable unredacted netlog flags to view).',
          ),
      m(
        'div',
        {
          style: {
            display: 'flex',
            justifyContent: 'space-between',
            alignItems: 'center',
            marginTop: '20px',
            marginBottom: '12px',
            borderBottom: '1px solid var(--pf-color-border)',
            paddingBottom: '6px',
          },
        },
        m(
          'h3',
          {style: {margin: 0, borderBottom: 'none', paddingBottom: 0}},
          'Request Headers',
        ),
        m(TextInput, {
          leftIcon: Icons.Search,
          placeholder: 'Filter request...',
          value: this.requestSearchQuery,
          onInput: (val: string) => {
            this.requestSearchQuery = val;
          },
        }),
      ),
      filteredRequest.length > 0
        ? renderMetaTable(filteredRequest)
        : m(
            'p',
            requestEntries.length > 0
              ? 'No request headers match your search filter.'
              : 'Request headers not recorded in this trace event.',
          ),
    );
  }
}

export class PreviewTab implements m.ClassComponent<DetailsTabAttrs> {
  view({attrs}: m.CVnode<DetailsTabAttrs>): m.Children {
    const {request} = attrs;
    return m(
      '.pf-network-details__content',
      m('h3', 'Response Preview'),
      request.mimeType && m('div', `MIME Type: ${request.mimeType}`),
      m(
        'p',
        'Payload preview is not available in standard timeline trace events. Enable unredacted network logging feature flags to capture raw bodies.',
      ),
    );
  }
}

export class ResponseTab implements m.ClassComponent<DetailsTabAttrs> {
  view({attrs}: m.CVnode<DetailsTabAttrs>): m.Children {
    const {request} = attrs;
    return m(
      '.pf-network-details__content',
      m('h3', 'Raw Response'),
      request.mimeType && m('div', `MIME Type: ${request.mimeType}`),
      m(
        'p',
        'Response payload not recorded in standard timeline events. Ensure netlog unredacted flags are enabled during capture.',
      ),
    );
  }
}

export class InitiatorTab implements m.ClassComponent<DetailsTabAttrs> {
  view({attrs}: m.CVnode<DetailsTabAttrs>): m.Children {
    const {request} = attrs;
    return m(
      '.pf-network-details__content',
      m('h3', 'Request Initiator'),
      m(
        'pre',
        request.initiator
          ? formatJsonPreview(request.initiator)
          : 'Initiator context not recorded in this trace event.',
      ),
    );
  }
}

export class TimingTab implements m.ClassComponent<DetailsTabAttrs> {
  view({attrs}: m.CVnode<DetailsTabAttrs>): m.Children {
    const {request} = attrs;
    const durationMs = Number(request.dur) / 1e6;
    const totalMs = Math.max(0.01, durationMs);

    const q = request.queueingTimeMs ?? totalMs * 0.1;
    const s = request.sendDurationMs ?? totalMs * 0.15;
    const w = request.waitingTimeMs ?? totalMs * 0.4;
    const d = request.downloadDurationMs ?? totalMs * 0.35;

    const queuePct = Math.min(100, Math.max(5, (q / totalMs) * 100));
    const sendPct = Math.min(100 - queuePct, Math.max(5, (s / totalMs) * 100));
    const waitPct = Math.min(100 - queuePct - sendPct, Math.max(5, (w / totalMs) * 100));
    const downloadPct = Math.max(0, 100 - queuePct - sendPct - waitPct);

    const stages = [
      {label: 'Queueing', val: q, pct: queuePct, left: 0, color: '#a8a8a8'},
      {label: 'Request Sent', val: s, pct: sendPct, left: queuePct, color: '#2563eb'},
      {label: 'Waiting (TTFB)', val: w, pct: waitPct, left: queuePct + sendPct, color: '#16a34a'},
      {label: 'Content Download', val: d, pct: downloadPct, left: queuePct + sendPct + waitPct, color: '#dc2626'},
    ];

    const renderedRows = stages.map((stage) => {
      return m(
        '.pf-network-timing-row',
        m('.pf-network-timing-row__label', stage.label),
        m(
          '.pf-network-timing-row__track-container',
          m('.pf-network-timing-row__bar', {
            style: {
              left: `${stage.left}%`,
              width: `${Math.min(100 - stage.left, Math.max(2, stage.pct))}%`,
              backgroundColor: stage.color,
            },
          }),
        ),
        m('.pf-network-timing-row__val', `${stage.val.toFixed(1)} ms`),
      );
    });

    const sizeEntries: [string, string][] = [
      ['Compressed Size', formatSize(request.encodedDataLength, 1) || '0 B'],
      ['Decoded Size', formatSize(request.decodedBodyLength, 1) || '0 B'],
      ['Memory Cached', request.servedFromMemoryCache ? 'Yes' : 'No'],
      ['Disk Cached', request.servedFromDiskCache ? 'Yes' : 'No'],
    ];

    return m(
      '.pf-network-details__content',
      m('h3', 'Timing Breakdown'),
      m(
        '.pf-network-timing-graphic',
        renderedRows,
        m(
          '.pf-network-timing-summary',
          m('span', 'Total Dur'),
          m('span', `${durationMs.toFixed(1)} ms`),
        ),
      ),
      m('h3', 'Size & Transfer Details'),
      renderMetaTable(sizeEntries),
    );
  }
}

export class CookiesTab implements m.ClassComponent<DetailsTabAttrs> {
  view({attrs}: m.CVnode<DetailsTabAttrs>): m.Children {
    const {request} = attrs;
    const requestCookies: [string, string][] = [];
    const responseCookies: [string, string][] = [];

    const {request: requestEntries, response: responseEntries} =
      parseRawHeaders(request.headers);

    for (const [k, v] of requestEntries) {
      if (k.toLowerCase() === 'cookie') {
        const parts = v.split(';');
        for (const p of parts) {
          const eq = p.indexOf('=');
          if (eq >= 0) {
            requestCookies.push([
              p.slice(0, eq).trim(),
              p.slice(eq + 1).trim(),
            ]);
          } else if (p.trim()) {
            requestCookies.push([p.trim(), '']);
          }
        }
      }
    }

    for (const [k, v] of responseEntries) {
      if (k.toLowerCase() === 'set-cookie') {
        const semi = v.indexOf(';');
        const primary = semi >= 0 ? v.slice(0, semi) : v;
        const eq = primary.indexOf('=');
        if (eq >= 0) {
          responseCookies.push([primary.slice(0, eq).trim(), v]);
        } else {
          responseCookies.push(['Set-Cookie', v]);
        }
      }
    }

    return m(
      '.pf-network-details__content',
      m('h3', 'Request Cookies'),
      requestCookies.length > 0
        ? renderMetaTable(requestCookies)
        : m('p', 'No request cookies recorded in this trace event.'),
      m('h3', 'Response Cookies'),
      responseCookies.length > 0
        ? renderMetaTable(responseCookies)
        : m('p', 'No response cookies recorded in this trace event.'),
    );
  }
}
