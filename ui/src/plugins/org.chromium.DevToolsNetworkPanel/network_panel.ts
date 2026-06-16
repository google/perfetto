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
import {classNames} from '../../base/classnames';
import {LONG, NUM, STR, STR_NULL, NUM_NULL, LONG_NULL, type Row} from '../../trace_processor/query_result';
import type {Trace} from '../../public/trace';
import {SplitPanel} from '../../widgets/split_panel';
import {Tabs} from '../../widgets/tabs';
import {EmptyState} from '../../widgets/empty_state';
import {Spinner} from '../../widgets/spinner';
import {Button} from '../../widgets/button';
import {TextInput} from '../../widgets/text_input';
import {Icons} from '../../base/semantic_icons';
import {Tooltip} from '../../widgets/tooltip';
import {PopupPosition} from '../../widgets/popup';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import type {SchemaRegistry} from '../../components/widgets/datagrid/datagrid_schema';
import {
  CookiesTab,
  HeadersTab,
  InitiatorTab,
  type NetworkRequest,
  PreviewTab,
  ResponseTab,
  TimingTab,
  formatSize,
  parseRawHeaders,
  highlightSliceAndZoom,
} from './details_tabs';

export interface NetworkPanelAttrs {
  readonly trace: Trace;
}

function normalizePriority(raw: string): string {
  if (!raw) return 'Medium';
  switch (raw.toLowerCase()) {
    case 'very_low':
    case 'verylow':
    case 'lowest':
      return 'Very Low';
    case 'low':
      return 'Low';
    case 'medium':
      return 'Medium';
    case 'high':
      return 'High';
    case 'very_high':
    case 'veryhigh':
    case 'highest':
      return 'Very High';
    default:
      return raw.charAt(0).toUpperCase() + raw.slice(1).toLowerCase();
  }
}

export class NetworkPanelView implements m.ClassComponent<NetworkPanelAttrs> {
  private loading = true;
  private requests: NetworkRequest[] = [];
  private requestMap = new Map<number, NetworkRequest>();
  private selectedRequestId?: number;
  private activeDetailTab = 'headers';
  private networkSearchQuery = '';
  private currentTrace?: Trace;

  // Dynamically populated schema registry for all extracted HTTP headers
  private dynamicHeadersSchema: Record<
    string,
    {
      title: string;
      columnType: 'text';
      cellRenderer: (val: unknown, row: Row) => unknown;
    }
  > = {};

  // Precalculated global timeline boundaries for O(1) waterfall scaling
  private minTs = 0n;
  private maxTs = 1n;
  private totalSpan = 1n;

  constructor({attrs}: m.CVnode<NetworkPanelAttrs>) {
    this.currentTrace = attrs.trace;
    this.loadNetworkRequests(attrs.trace);
  }

  private async loadNetworkRequests(trace: Trace): Promise<void> {
    const query = `
      WITH network_start AS (
        SELECT 
          slice.id,
          slice.ts,
          slice.dur,
          slice.arg_set_id,
          COALESCE(
            EXTRACT_ARG(slice.arg_set_id, 'debug.data.requestId'),
            EXTRACT_ARG(slice.arg_set_id, 'data.requestId'),
            EXTRACT_ARG(slice.arg_set_id, 'debug.params.source_start_time'),
            EXTRACT_ARG(slice.arg_set_id, 'source_start_time')
          ) AS req_id,
          COALESCE(
            EXTRACT_ARG(slice.arg_set_id, 'debug.data.url'),
            EXTRACT_ARG(slice.arg_set_id, 'debug.params.url'),
            EXTRACT_ARG(slice.arg_set_id, 'debug.url'),
            EXTRACT_ARG(slice.arg_set_id, 'data.url'), 
            EXTRACT_ARG(slice.arg_set_id, 'url'), 
            EXTRACT_ARG(slice.arg_set_id, 'request.url'),
            EXTRACT_ARG(slice.arg_set_id, 'request_url'),
            slice.name
          ) AS url
        FROM slice
        WHERE (slice.category LIKE '%devtools.timeline%' AND slice.name = 'ResourceSendRequest')
           OR ((slice.category LIKE '%net%' OR slice.category LIKE '%netlog%' OR slice.category LIKE '%network%') 
               AND slice.name IN ('URL_REQUEST', 'URL_REQUEST_ALIVE', 'URLLoader', 'CORS_REQUEST'))
      ),
      network_finish_raw AS (
        SELECT 
          slice.name,
          slice.ts,
          slice.dur,
          slice.arg_set_id,
          COALESCE(
            EXTRACT_ARG(slice.arg_set_id, 'debug.data.requestId'),
            EXTRACT_ARG(slice.arg_set_id, 'data.requestId'),
            EXTRACT_ARG(slice.arg_set_id, 'debug.params.source_start_time'),
            EXTRACT_ARG(slice.arg_set_id, 'source_start_time')
          ) AS req_id
        FROM slice
        WHERE slice.category LIKE '%devtools.timeline%' 
           OR slice.category LIKE '%net%' 
           OR slice.category LIKE '%netlog%' 
           OR slice.category LIKE '%network%'
      ),
      network_finish AS (
        SELECT 
          req_id,
          MAX(COALESCE(
            EXTRACT_ARG(arg_set_id, 'debug.data.encodedDataLength'),
            EXTRACT_ARG(arg_set_id, 'data.encodedDataLength'),
            EXTRACT_ARG(arg_set_id, 'debug.params.packet_length'),
            EXTRACT_ARG(arg_set_id, 'encodedDataLength')
          )) AS encodedDataLength,
          MAX(COALESCE(
            EXTRACT_ARG(arg_set_id, 'debug.data.decodedBodyLength'),
            EXTRACT_ARG(arg_set_id, 'data.decodedBodyLength'), 
            EXTRACT_ARG(arg_set_id, 'data.dataLength'), 
            EXTRACT_ARG(arg_set_id, 'decodedBodyLength')
          )) AS decodedBodyLength,
          MAX(CASE WHEN name = 'ResourceMarkAsCached' THEN 1 ELSE 0 END) AS servedFromMemoryCache,
          MAX(CASE WHEN name IN ('HttpCache::OnExternalCacheHit', 'DoCacheReadResponse') 
                     OR name LIKE '%CacheRead%' 
                   THEN 1 ELSE 0 END) AS servedFromDiskCache,
          MIN(CASE WHEN name = 'HTTP_TRANSACTION_SEND_REQUEST' THEN ts ELSE NULL END) AS netlog_send_ts,
          MAX(CASE WHEN name = 'HTTP_TRANSACTION_SEND_REQUEST' THEN dur ELSE NULL END) AS netlog_send_dur,
          MIN(CASE WHEN name IN ('HTTP_TRANSACTION_READ_RESPONSE_HEADERS', 'NETWORK_DELEGATE_HEADERS_RECEIVED') THEN ts ELSE NULL END) AS netlog_headers_ts,
          GROUP_CONCAT(__intrinsic_arg_set_to_json(arg_set_id), '|||') AS raw_headers_list
        FROM network_finish_raw
        WHERE req_id IS NOT NULL
        GROUP BY req_id
      )
      SELECT 
        s.id,
        s.ts,
        s.dur,
        s.url,
        COALESCE(
          EXTRACT_ARG(s.arg_set_id, 'debug.data.requestMethod'),
          EXTRACT_ARG(s.arg_set_id, 'debug.params.method'),
          EXTRACT_ARG(s.arg_set_id, 'debug.method'),
          EXTRACT_ARG(s.arg_set_id, 'data.requestMethod'), 
          EXTRACT_ARG(s.arg_set_id, 'method'), 
          'GET'
        ) AS method,
        COALESCE(
          EXTRACT_ARG(s.arg_set_id, 'debug.data.statusCode'),
          EXTRACT_ARG(s.arg_set_id, 'debug.params.status_code'),
          EXTRACT_ARG(s.arg_set_id, 'debug.status_code'),
          EXTRACT_ARG(s.arg_set_id, 'data.statusCode'), 
          EXTRACT_ARG(s.arg_set_id, 'status_code'), 
          200
        ) AS statusCode,
        COALESCE(
          EXTRACT_ARG(s.arg_set_id, 'debug.data.priority'),
          EXTRACT_ARG(s.arg_set_id, 'debug.params.priority'),
          EXTRACT_ARG(s.arg_set_id, 'debug.priority'),
          EXTRACT_ARG(s.arg_set_id, 'data.priority'), 
          EXTRACT_ARG(s.arg_set_id, 'priority'), 
          'Medium'
        ) AS priority,
        COALESCE(
          EXTRACT_ARG(s.arg_set_id, 'debug.data.resourceType'),
          EXTRACT_ARG(s.arg_set_id, 'debug.data.mimeType'),
          EXTRACT_ARG(s.arg_set_id, 'debug.params.request_type'),
          EXTRACT_ARG(s.arg_set_id, 'debug.params.mime_type'),
          EXTRACT_ARG(s.arg_set_id, 'data.mimeType'), 
          EXTRACT_ARG(s.arg_set_id, 'mime_type'), 
          'document'
        ) AS mimeType,
        COALESCE(
          EXTRACT_ARG(s.arg_set_id, 'debug.data.initiator'),
          EXTRACT_ARG(s.arg_set_id, 'debug.params.initiator'),
          EXTRACT_ARG(s.arg_set_id, 'data.initiator'), 
          EXTRACT_ARG(s.arg_set_id, 'initiator')
        ) AS initiator,
        COALESCE(
          EXTRACT_ARG(s.arg_set_id, 'debug.data.timing'),
          EXTRACT_ARG(s.arg_set_id, 'data.timing'), 
          __intrinsic_arg_set_to_json(s.arg_set_id)
        ) AS timing,
        COALESCE(
          EXTRACT_ARG(s.arg_set_id, 'debug.data.timing.sendStart'),
          EXTRACT_ARG(s.arg_set_id, 'data.timing.sendStart')
        ) AS timingSendStart,
        COALESCE(
          EXTRACT_ARG(s.arg_set_id, 'debug.data.timing.sendEnd'),
          EXTRACT_ARG(s.arg_set_id, 'data.timing.sendEnd')
        ) AS timingSendEnd,
        COALESCE(
          EXTRACT_ARG(s.arg_set_id, 'debug.data.timing.receiveHeadersEnd'),
          EXTRACT_ARG(s.arg_set_id, 'data.timing.receiveHeadersEnd')
        ) AS timingReceiveHeadersEnd,
        COALESCE(
          EXTRACT_ARG(s.arg_set_id, 'debug.data.protocol'),
          EXTRACT_ARG(s.arg_set_id, 'debug.params.protocol'),
          EXTRACT_ARG(s.arg_set_id, 'debug.protocol'),
          EXTRACT_ARG(s.arg_set_id, 'data.protocol'), 
          EXTRACT_ARG(s.arg_set_id, 'protocol'), 
          'h2'
        ) AS protocol,
        COALESCE(
          f.encodedDataLength,
          EXTRACT_ARG(s.arg_set_id, 'debug.data.encodedDataLength'),
          EXTRACT_ARG(s.arg_set_id, 'debug.params.packet_length'),
          EXTRACT_ARG(s.arg_set_id, 'data.encodedDataLength'), 
          EXTRACT_ARG(s.arg_set_id, 'encodedDataLength')
        ) AS encodedDataLength,
        COALESCE(
          f.decodedBodyLength,
          EXTRACT_ARG(s.arg_set_id, 'debug.data.decodedBodyLength'),
          EXTRACT_ARG(s.arg_set_id, 'data.decodedBodyLength'), 
          EXTRACT_ARG(s.arg_set_id, 'data.dataLength'), 
          EXTRACT_ARG(s.arg_set_id, 'decodedBodyLength')
        ) AS decodedBodyLength,
        COALESCE(f.servedFromMemoryCache, 0) AS servedFromMemoryCache,
        COALESCE(f.servedFromDiskCache, 0) AS servedFromDiskCache,
        f.netlog_send_ts AS netlogSendTs,
        f.netlog_send_dur AS netlogSendDur,
        f.netlog_headers_ts AS netlogHeadersTs,
        f.raw_headers_list AS headers
      FROM network_start s
      LEFT JOIN network_finish f ON s.req_id = f.req_id
      ORDER BY s.ts;
    `;

    try {
      const result = await trace.engine.query(query);
      const it = result.iter({
        id: NUM,
        ts: LONG,
        dur: LONG,
        url: STR,
        method: STR,
        statusCode: NUM,
        priority: STR,
        mimeType: STR_NULL,
        initiator: STR_NULL,
        timing: STR_NULL,
        timingSendStart: NUM_NULL,
        timingSendEnd: NUM_NULL,
        timingReceiveHeadersEnd: NUM_NULL,
        protocol: STR_NULL,
        encodedDataLength: NUM_NULL,
        decodedBodyLength: NUM_NULL,
        servedFromMemoryCache: NUM,
        servedFromDiskCache: NUM,
        netlogSendTs: LONG_NULL,
        netlogSendDur: LONG_NULL,
        netlogHeadersTs: LONG_NULL,
        headers: STR_NULL,
      });

      for (; it.valid(); it.next()) {
        const durMs = Number(it.dur) / 1e6;
        const startTimeMs = Number(it.ts) / 1e6;
        let queueingTimeMs: number;
        let sendDurationMs: number;
        let waitingTimeMs: number;
        let downloadDurationMs: number;

        const sStart =
          it.timingSendStart !== null ? Number(it.timingSendStart) : -1;
        const sEnd = it.timingSendEnd !== null ? Number(it.timingSendEnd) : -1;
        const rHeadersEnd =
          it.timingReceiveHeadersEnd !== null
            ? Number(it.timingReceiveHeadersEnd)
            : -1;

        if (sStart >= 0 && sEnd >= sStart && rHeadersEnd >= sEnd) {
          // High-fidelity Blink ResourceLoadTiming
          queueingTimeMs = sStart;
          sendDurationMs = sEnd - sStart;
          waitingTimeMs = rHeadersEnd - sEnd;
          downloadDurationMs = Math.max(0, durMs - rHeadersEnd);
        } else {
          // Pure NetLog milestones based on exact timestamp differences!
          const startTs = Number(it.ts);
          const endTs = Number(it.ts + it.dur);
          const netlogSendTs = it.netlogSendTs !== null && it.netlogSendTs !== 0n ? Number(it.netlogSendTs) : startTs;
          const netlogSendDur = it.netlogSendDur !== null && it.netlogSendDur !== 0n ? Number(it.netlogSendDur) : 0;
          const netlogHeadersTs = it.netlogHeadersTs !== null && it.netlogHeadersTs !== 0n ? Number(it.netlogHeadersTs) : endTs;

          queueingTimeMs = Math.max(0, (netlogSendTs - startTs) / 1e6);
          sendDurationMs = netlogSendDur / 1e6;
          waitingTimeMs = Math.max(0, (netlogHeadersTs - (netlogSendTs + netlogSendDur)) / 1e6);
          downloadDurationMs = Math.max(0, (endTs - netlogHeadersTs) / 1e6);
        }

        let encLen = it.encodedDataLength !== null ? Number(it.encodedDataLength) : null;
        const decLen = it.decodedBodyLength !== null ? Number(it.decodedBodyLength) : null;

        let parsedHeaders: {request: [string, string][]; response: [string, string][]} | null = null;
        if (it.headers) {
          parsedHeaders = parseRawHeaders(it.headers);
          if (encLen === null) {
            for (const [k, v] of parsedHeaders.response) {
              if (k.toLowerCase() === 'content-length') {
                const parsed = parseInt(v, 10);
                if (!isNaN(parsed)) {
                  encLen = parsed;
                  break;
                }
              }
            }
          }
        }

        const servedFromMemory = it.servedFromMemoryCache > 0;
        const servedFromDisk = it.servedFromDiskCache > 0 || (encLen === 0 && decLen !== null && decLen > 0);

        let transferredStr = encLen !== null ? formatSize(encLen, 1) : '0 B';
        let transferredNum = encLen !== null ? encLen : 0;
        if (servedFromMemory) {
          transferredStr = '(memory cache)';
          transferredNum = 0;
        } else if (servedFromDisk) {
          transferredStr = '(disk cache)';
          transferredNum = 0;
        }

        const req: NetworkRequest = {
          id: it.id,
          ts: it.ts,
          dur: it.dur,
          url: it.url,
          name: this.formatName(it.url),
          domain: this.formatDomain(it.url),
          method: it.method,
          statusCode: it.statusCode,
          priority: normalizePriority(it.priority),
          mimeType: it.mimeType,
          protocol: it.protocol,
          encodedDataLength: encLen,
          decodedBodyLength: decLen,
          servedFromMemoryCache: it.servedFromMemoryCache,
          servedFromDiskCache: servedFromDisk ? 1 : 0,
          startTimeMs,
          durationMs: durMs,
          totalDurMs: durMs,
          queueingTimeMs,
          sendDurationMs,
          waitingTimeMs,
          downloadDurationMs,
          headers: it.headers,
          initiator: it.initiator,
          timing: it.timing,
          timeline: String(it.id),
          transferred: transferredNum,
          transferredStr,
        };

        if (parsedHeaders) {
          for (const [k, v] of parsedHeaders.request) {
            this.registerDynamicHeader(req, 'Req', k, v);
          }
          for (const [k, v] of parsedHeaders.response) {
            this.registerDynamicHeader(req, 'Resp', k, v);
          }
        }

        this.requests.push(req);
        this.requestMap.set(req.id, req);
      }

      if (this.requests.length > 0) {
        this.minTs = this.requests[0]?.ts ?? 0n;
        this.maxTs = this.requests[0]?.ts ?? 1n;
        for (const r of this.requests) {
          if (r.ts < this.minTs) this.minTs = r.ts;
          const end = r.ts + r.dur;
          if (end > this.maxTs) this.maxTs = end;
        }
        this.totalSpan = this.maxTs - this.minTs;
      }
    } finally {
      this.loading = false;
      m.redraw();
    }
  }

  private registerDynamicHeader(
    row: NetworkRequest,
    prefix: string,
    key: string,
    val: string,
  ): void {
    const schemaKey = `${prefix.toLowerCase()}_${key.toLowerCase().replace(/[^a-z0-9]/g, '_')}`;
    (row as unknown as Record<string, unknown>)[schemaKey] = val;
    if (!this.dynamicHeadersSchema[schemaKey]) {
      this.dynamicHeadersSchema[schemaKey] = {
        title: `${prefix}: ${key}`,
        columnType: 'text',
        cellRenderer: (cellVal, cellRow) =>
          this.renderCellWithSelection(cellVal, cellRow, this.currentTrace!),
      };
    }
  }

  private findRequestByRow(row: Row): NetworkRequest | undefined {
    if (row['id'] !== undefined && row['id'] !== null) {
      return this.requestMap.get(Number(row['id']));
    }
    for (const key of Object.keys(row)) {
      const val = row[key];
      if (val !== undefined && val !== null) {
        const found = this.requests.find(
          (r) => (r as unknown as Record<string, unknown>)[key] === val,
        );
        if (found) return found;
      }
    }
    return this.requests[0];
  }

  private handleSelectRequest(req: NetworkRequest): void {
    this.selectedRequestId = req.id;
  }

  view({attrs}: m.CVnode<NetworkPanelAttrs>): m.Children {
    if (this.loading) {
      return m(
        '.pf-network-panel',
        m(EmptyState, {title: 'Loading Network Requests...'}, m(Spinner)),
      );
    }

    if (this.requests.length === 0) {
      return m(
        '.pf-network-panel',
        m(EmptyState, {
          title: 'No Network Requests Recorded',
          subtitle:
            'Ensure devtools.timeline or netlog trace categories are enabled during capture.',
        }),
      );
    }

    const selectedRequest =
      this.selectedRequestId !== undefined
        ? this.requestMap.get(this.selectedRequestId)
        : undefined;

    const query = this.networkSearchQuery.trim().toLowerCase();
    const filteredRequests = query
      ? this.requests.filter((req) => {
          return (
            req.url.toLowerCase().includes(query) ||
            req.name.toLowerCase().includes(query) ||
            req.method.toLowerCase().includes(query) ||
            String(req.statusCode).includes(query) ||
            req.domain.toLowerCase().includes(query) ||
            (req.protocol ?? '').toLowerCase().includes(query) ||
            (req.mimeType ?? '').toLowerCase().includes(query) ||
            req.priority.toLowerCase().includes(query)
          );
        })
      : this.requests;

    const schema: SchemaRegistry = {
      request: {
        id: {
          title: 'ID',
          columnType: 'quantitative',
          cellRenderer: (val, row) =>
            this.renderCellWithSelection(val, row, attrs.trace),
        },
        timeline: {
          title: 'Timeline',
          columnType: 'text',
          cellRenderer: (_, row) => this.renderWaterfallCell(row, attrs.trace),
        },
        name: {
          title: 'Name',
          columnType: 'text',
          cellRenderer: (val, row) =>
            this.renderNameCell(val, row, attrs.trace),
        },
        statusCode: {
          title: 'Status',
          columnType: 'quantitative',
          cellRenderer: (val, row) =>
            this.renderCellWithSelection(val, row, attrs.trace),
        },
        method: {
          title: 'Method',
          columnType: 'text',
          distinctValues: true,
          cellRenderer: (val, row) =>
            this.renderCellWithSelection(val, row, attrs.trace),
        },
        priority: {
          title: 'Priority',
          columnType: 'text',
          distinctValues: true,
          cellRenderer: (val, row) =>
            this.renderCellWithSelection(val, row, attrs.trace),
        },
        mimeType: {
          title: 'Type',
          columnType: 'text',
          distinctValues: true,
          cellRenderer: (val, row) =>
            this.renderCellWithSelection(val, row, attrs.trace),
        },
        protocol: {
          title: 'Protocol',
          columnType: 'text',
          distinctValues: true,
          cellRenderer: (val, row) =>
            this.renderCellWithSelection(val, row, attrs.trace),
        },
        domain: {
          title: 'Domain',
          columnType: 'text',
          cellRenderer: (val, row) =>
            this.renderCellWithSelection(val, row, attrs.trace),
        },
        transferred: {
          title: 'Transferred Size',
          columnType: 'quantitative',
          cellRenderer: (_val, row) => {
            const req = this.findRequestByRow(row);
            return this.renderCellWithSelection(
              req?.transferredStr ?? '',
              row,
              attrs.trace,
              true,
            );
          },
        },
        encodedDataLength: {
          title: 'Compressed Size',
          columnType: 'quantitative',
          cellRenderer: (val, row) =>
            this.renderCellWithSelection(
              formatSize(val, 1),
              row,
              attrs.trace,
              true,
            ),
        },
        decodedBodyLength: {
          title: 'Decoded Size',
          columnType: 'quantitative',
          cellRenderer: (val, row) =>
            this.renderCellWithSelection(
              formatSize(val, 1),
              row,
              attrs.trace,
              true,
            ),
        },
        servedFromMemoryCache: {
          title: 'Memory Cached',
          columnType: 'text',
          distinctValues: true,
          cellRenderer: (val, row) =>
            this.renderCellWithSelection(
              val ? 'Yes' : 'No',
              row,
              attrs.trace,
            ),
        },
        servedFromDiskCache: {
          title: 'Disk Cached',
          columnType: 'text',
          distinctValues: true,
          cellRenderer: (val, row) =>
            this.renderCellWithSelection(
              val ? 'Yes' : 'No',
              row,
              attrs.trace,
            ),
        },
        startTimeMs: {
          title: 'Start Time',
          columnType: 'quantitative',
          cellRenderer: (val, row) =>
            this.renderCellWithSelection(
              typeof val === 'number' ? `${val.toFixed(1)} ms` : '',
              row,
              attrs.trace,
              true,
            ),
        },
        durationMs: {
          title: 'Dur',
          columnType: 'quantitative',
          cellRenderer: (val, row) =>
            this.renderCellWithSelection(
              typeof val === 'number' ? `${val.toFixed(1)} ms` : '',
              row,
              attrs.trace,
              true,
            ),
        },
        totalDurMs: {
          title: 'Total Dur',
          columnType: 'quantitative',
          cellRenderer: (val, row) =>
            this.renderCellWithSelection(
              typeof val === 'number' ? `${val.toFixed(1)} ms` : '',
              row,
              attrs.trace,
              true,
            ),
        },
        queueingTimeMs: {
          title: 'Queuing Dur',
          columnType: 'quantitative',
          cellRenderer: (val, row) =>
            this.renderCellWithSelection(
              typeof val === 'number' ? `${val.toFixed(1)} ms` : '0 ms',
              row,
              attrs.trace,
              true,
            ),
        },
        sendDurationMs: {
          title: 'Send Dur',
          columnType: 'quantitative',
          cellRenderer: (val, row) =>
            this.renderCellWithSelection(
              typeof val === 'number' ? `${val.toFixed(1)} ms` : '0 ms',
              row,
              attrs.trace,
              true,
            ),
        },
        waitingTimeMs: {
          title: 'Waiting Dur',
          columnType: 'quantitative',
          cellRenderer: (val, row) =>
            this.renderCellWithSelection(
              typeof val === 'number' ? `${val.toFixed(1)} ms` : '0 ms',
              row,
              attrs.trace,
              true,
            ),
        },
        downloadDurationMs: {
          title: 'Download Dur',
          columnType: 'quantitative',
          cellRenderer: (val, row) =>
            this.renderCellWithSelection(
              typeof val === 'number' ? `${val.toFixed(1)} ms` : '0 ms',
              row,
              attrs.trace,
              true,
            ),
        },
        url: {
          title: 'URL',
          columnType: 'text',
          cellRenderer: (val, row) =>
            this.renderCellWithSelection(val, row, attrs.trace),
        },
        ...this.dynamicHeadersSchema,
      },
    };

    return m(
      '.pf-network-panel',
      m(SplitPanel, {
        direction: 'horizontal',
        initialSplit: {percent: 60},
        minSize: 200,
        className: 'pf-network-split',
        firstPanel: m(
          '.pf-network-table-container',
          m(DataGrid, {
            fillHeight: true,
            schema,
            rootSchema: 'request',
            data: filteredRequests,
            toolbarItemsLeft: m(TextInput, {
              leftIcon: Icons.Search,
              placeholder: 'Search requests...',
              value: this.networkSearchQuery,
              onInput: (val: string) => {
                this.networkSearchQuery = val;
              },
            }),
            initialColumns: [
              {id: 'id', field: 'id'},
              {id: 'timeline', field: 'timeline'},
              {id: 'name', field: 'name'},
              {id: 'statusCode', field: 'statusCode'},
              {id: 'method', field: 'method'},
              {id: 'priority', field: 'priority'},
              {id: 'domain', field: 'domain'},
              {id: 'transferred', field: 'transferred'},
              {id: 'durationMs', field: 'durationMs'},
            ],
          }),
        ),
        secondPanel: m(
          '.pf-network-details',
          selectedRequest
            ? m(Tabs, {
                activeTabKey: this.activeDetailTab,
                onTabChange: (key: string) => {
                  this.activeDetailTab = key;
                },
                tabs: [
                  {
                    key: 'headers',
                    title: 'Headers',
                    content: m(HeadersTab, {
                      request: selectedRequest,
                      trace: attrs.trace,
                    }),
                  },
                  {
                    key: 'preview',
                    title: 'Preview',
                    content: m(PreviewTab, {
                      request: selectedRequest,
                      trace: attrs.trace,
                    }),
                  },
                  {
                    key: 'response',
                    title: 'Response',
                    content: m(ResponseTab, {
                      request: selectedRequest,
                      trace: attrs.trace,
                    }),
                  },
                  {
                    key: 'initiator',
                    title: 'Initiator',
                    content: m(InitiatorTab, {
                      request: selectedRequest,
                      trace: attrs.trace,
                    }),
                  },
                  {
                    key: 'timing',
                    title: 'Timing',
                    content: m(TimingTab, {
                      request: selectedRequest,
                      trace: attrs.trace,
                    }),
                  },
                  {
                    key: 'cookies',
                    title: 'Cookies',
                    content: m(CookiesTab, {
                      request: selectedRequest,
                      trace: attrs.trace,
                    }),
                  },
                ],
              })
            : m(EmptyState, {title: 'No request selected'}),
        ),
      }),
    );
  }

  private renderCellContainer(
    req: NetworkRequest | undefined,
    _trace: Trace,
    content: m.Children,
    style?: Partial<CSSStyleDeclaration>,
  ) {
    const reqId = req?.id;
    const isSelected = reqId !== undefined && this.selectedRequestId === reqId;
    return {
      content: m(
        'div.pf-network-datagrid-cell',
        {
          style,
          className: classNames(
            isSelected && 'pf-network-datagrid-cell--selected',
          ),
          onclick: (e: Event) => {
            e.stopPropagation();
            if (req !== undefined) {
              this.handleSelectRequest(req);
            }
          },
        },
        content,
      ),
    };
  }

  private renderCellWithSelection(
    value: unknown,
    row: Row,
    trace: Trace,
    rightAlign = false,
  ) {
    const req = this.findRequestByRow(row);
    return this.renderCellContainer(
      req,
      trace,
      String(value ?? ''),
      rightAlign
        ? {
            justifyContent: 'flex-end',
            fontVariantNumeric: 'tabular-nums',
            width: '100%',
          }
        : {width: '100%'},
    );
  }

  private renderNameCell(val: unknown, row: Row, trace: Trace) {
    const req = this.findRequestByRow(row);
    const fullUrl = req?.url ?? String(val ?? '');

    return this.renderCellContainer(
      req,
      trace,
      m(
        Tooltip,
        {
          trigger: m(
            'div',
            {
              style: {
                width: '100%',
                overflow: 'hidden',
                textOverflow: 'ellipsis',
              },
            },
            String(val ?? ''),
          ),
          position: PopupPosition.Right,
          fitContent: true,
        },
        m('.pf-network-url-tooltip', fullUrl),
      ),
    );
  }

  private renderWaterfallCell(row: Row, trace: Trace) {
    const req = this.findRequestByRow(row);
    if (!req) return {content: m('div')};

    const startRel = req.ts - this.minTs;
    const offsetPercent =
      this.totalSpan > 0n
        ? (Number(startRel) / Number(this.totalSpan)) * 100
        : 0;
    const durPercent =
      this.totalSpan > 0n
        ? (Number(req.dur) / Number(this.totalSpan)) * 100
        : 100;
    const clampedOffset = Math.min(95, Math.max(0, offsetPercent));
    const clampedWidth = Math.max(3, Math.min(100 - clampedOffset, durPercent));

    const durationMs = Number(req.dur) / 1e6;
    const totalMs = Math.max(0.01, durationMs);

    const queueMs = req.queueingTimeMs ?? totalMs * 0.1;
    const sendMs = req.sendDurationMs ?? totalMs * 0.15;
    const waitMs = req.waitingTimeMs ?? totalMs * 0.4;
    const downloadMs = req.downloadDurationMs ?? totalMs * 0.35;

    const queuePct = Math.min(100, Math.max(5, (queueMs / totalMs) * 100));
    const sendPct = Math.min(100 - queuePct, Math.max(5, (sendMs / totalMs) * 100));
    const waitPct = Math.min(100 - queuePct - sendPct, Math.max(5, (waitMs / totalMs) * 100));
    const downloadPct = Math.max(0, 100 - queuePct - sendPct - waitPct);

    const stages = [
      {label: 'Queuing', ms: queueMs, pct: queuePct, left: 0, color: '#a8a8a8', bgColor: 'rgba(168, 168, 168, 0.25)'},
      {label: 'Request Sending', ms: sendMs, pct: sendPct, left: queuePct, color: '#2563eb', bgColor: 'rgba(37, 99, 235, 0.25)'},
      {label: 'Waiting (TTFB)', ms: waitMs, pct: waitPct, left: queuePct + sendPct, color: '#16a34a', bgColor: 'rgba(22, 163, 74, 0.25)'},
      {label: 'Downloading', ms: downloadMs, pct: downloadPct, left: queuePct + sendPct + waitPct, color: '#dc2626', bgColor: 'rgba(220, 38, 38, 0.25)'},
    ];

    const tooltipContent = m(
      '.pf-network-timeline-tooltip',
      stages.map((st) =>
        m(
          '.pf-network-timeline-tooltip__row',
          m('.pf-network-timeline-tooltip__bg', {
            style: {
              left: `${st.left}%`,
              width: `${Math.min(100 - st.left, Math.max(2, st.pct))}%`,
              backgroundColor: st.bgColor,
            },
          }),
          m(
            '.pf-network-timeline-tooltip__content',
            m('span.pf-network-timeline-tooltip__swatch', {
              style: {backgroundColor: st.color},
            }),
            m('span.pf-network-timeline-tooltip__label', st.label),
          ),
          m('.pf-network-timeline-tooltip__val', `${st.ms.toFixed(1)} ms`),
        ),
      ),
      m(
        '.pf-network-timeline-tooltip__footer',
        m('span', 'Total Dur'),
        m('span', `${durationMs.toFixed(1)} ms`),
      ),
    );

    const triggerDiv = m(
      'div',
      {
        style: {flexGrow: 1, display: 'flex', alignItems: 'center'},
      },
      m(
        '.pf-network-waterfall',
        m(
          '.pf-network-waterfall__track',
          {
            style: {
              left: `${clampedOffset}%`,
              width: `${clampedWidth}%`,
            },
          },
          m('.pf-network-waterfall__queue', {
            style: {flex: `0 0 ${queuePct}%`},
          }),
          m('.pf-network-waterfall__send', {style: {flex: `0 0 ${sendPct}%`}}),
          m('.pf-network-waterfall__wait', {style: {flex: `0 0 ${waitPct}%`}}),
          m('.pf-network-waterfall__download', {
            style: {flex: `0 0 ${downloadPct}%`},
          }),
        ),
      ),
    );

    return this.renderCellContainer(
      req,
      trace,
      [
        m(
          Tooltip,
          {
            trigger: triggerDiv,
            position: PopupPosition.Bottom,
            fitContent: true,
          },
          tooltipContent,
        ),
        m(Button, {
          className: 'pf-visible-on-hover',
          compact: true,
          icon: Icons.UpdateSelection,
          tooltip: 'Highlight slice and zoom time range in track view',
          onclick: (e: Event) => {
            e.stopPropagation();
            this.handleSelectRequest(req);
            highlightSliceAndZoom(trace, req);
          },
        }),
      ],
      {display: 'flex', alignItems: 'center', gap: '8px'},
    );
  }

  private formatName(url: string): string {
    try {
      const parsed = new URL(url);
      let path =
        parsed.pathname.split('/').filter(Boolean).pop() ?? parsed.host;
      if (parsed.search) {
        path += parsed.search;
      }
      return path;
    } catch {
      return url;
    }
  }

  private formatDomain(url: string): string {
    try {
      const parsed = new URL(url);
      if (
        parsed.protocol === 'chrome:' ||
        parsed.protocol === 'chrome-extension:' ||
        parsed.protocol === 'about:' ||
        parsed.protocol === 'data:'
      ) {
        return 'internal';
      }
      return parsed.host;
    } catch {
      return 'localhost';
    }
  }
}
