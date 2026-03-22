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

import {AdbWebusbDevice} from '../dev.perfetto.RecordTraceV2/adb/webusb/adb_webusb_device';
import {AdbKeyManager} from '../dev.perfetto.RecordTraceV2/adb/webusb/adb_key_manager';
import {ADB_DEVICE_FILTER} from '../dev.perfetto.RecordTraceV2/adb/webusb/adb_webusb_utils';
import {utf8Encode} from '../../base/string_utils';

// ── Types ───────────────────────────────────────────────────────────────────

export interface ProcessInfo {
  pid: number;
  name: string;
  oomLabel: string;
  pssKb: number;
  rssKb: number;
}

export interface SmapsEntry {
  addrStart: string;
  addrEnd: string;
  perms: string;
  name: string;
  dev: string;
  inode: number;
  sizeKb: number;
  rssKb: number;
  pssKb: number;
  sharedCleanKb: number;
  sharedDirtyKb: number;
  privateCleanKb: number;
  privateDirtyKb: number;
  swapKb: number;
  swapPssKb: number;
}

export interface VmaString {
  offset: number;
  vmaAddr: number;
  str: string;
  vmaIndex: number;
}

export interface VmaRegionInfo {
  addrStart: string;
  addrEnd: string;
  perms: string;
  name: string;
  sizeKb: number;
  stringCount: number;
}

export interface ProcessStringsResult {
  pid: number;
  processName: string;
  regions: VmaRegionInfo[];
  strings: VmaString[];
  scanning?: boolean;
  scannedVmas?: number;
  totalVmas?: number;
}

function parseGrepOutput(output: string): {offset: number; str: string}[] {
  const results: {offset: number; str: string}[] = [];
  for (const line of output.split('\n')) {
    const trimmed = line.replace(/\r$/, '');
    if (trimmed === '') continue;
    const colonIdx = trimmed.indexOf(':');
    if (colonIdx < 0) continue;
    const offset = parseInt(trimmed.substring(0, colonIdx), 10);
    if (!isFinite(offset)) continue;
    const str = trimmed.substring(colonIdx + 1);
    if (str.length >= 4) results.push({offset, str});
  }
  return results;
}

export interface SmapsAggregated {
  name: string;
  count: number;
  sizeKb: number;
  rssKb: number;
  pssKb: number;
  sharedCleanKb: number;
  sharedDirtyKb: number;
  privateCleanKb: number;
  privateDirtyKb: number;
  swapKb: number;
  swapPssKb: number;
  entries: SmapsEntry[];
}

// ── Shell helper ────────────────────────────────────────────────────────────

/** Run a shell command via Perfetto's ADB device and return stdout as string. */
async function shell(device: AdbWebusbDevice, cmd: string): Promise<string> {
  const result = await device.shell(cmd);
  if (!result.ok) throw new Error(result.error);
  return result.value;
}

// ── Smaps parsing ───────────────────────────────────────────────────────────

const VMA_HEADER =
  /^([0-9a-f]+)-([0-9a-f]+)\s+(\S+)\s+\S+\s+(\S+)\s+(\d+)\s*(.*)/;

export function parseSmaps(output: string): SmapsEntry[] {
  const entries: SmapsEntry[] = [];
  let cur: SmapsEntry | null = null;
  for (const line of output.split('\n')) {
    const hdr = VMA_HEADER.exec(line);
    if (hdr) {
      if (cur) entries.push(cur);
      cur = {
        addrStart: hdr[1],
        addrEnd: hdr[2],
        perms: hdr[3],
        dev: hdr[4],
        inode: parseInt(hdr[5], 10) || 0,
        name: (hdr[6] ?? '').trim(),
        sizeKb: 0,
        rssKb: 0,
        pssKb: 0,
        sharedCleanKb: 0,
        sharedDirtyKb: 0,
        privateCleanKb: 0,
        privateDirtyKb: 0,
        swapKb: 0,
        swapPssKb: 0,
      };
      continue;
    }
    if (!cur) continue;
    const m = /^(\w[\w_]*):\s+(\d+)\s+kB/.exec(line);
    if (!m) continue;
    const val = parseInt(m[2], 10);
    switch (m[1]) {
      case 'Size':
        cur.sizeKb = val;
        break;
      case 'Rss':
        cur.rssKb = val;
        break;
      case 'Pss':
        cur.pssKb = val;
        break;
      case 'Shared_Clean':
        cur.sharedCleanKb = val;
        break;
      case 'Shared_Dirty':
        cur.sharedDirtyKb = val;
        break;
      case 'Private_Clean':
        cur.privateCleanKb = val;
        break;
      case 'Private_Dirty':
        cur.privateDirtyKb = val;
        break;
      case 'Swap':
        cur.swapKb = val;
        break;
      case 'SwapPss':
        cur.swapPssKb = val;
        break;
    }
  }
  if (cur) entries.push(cur);
  return entries;
}

export function aggregateSmaps(entries: SmapsEntry[]): SmapsAggregated[] {
  const groups = new Map<string, SmapsAggregated>();
  for (const e of entries) {
    const existing = groups.get(e.name);
    if (existing) {
      existing.count++;
      existing.sizeKb += e.sizeKb;
      existing.rssKb += e.rssKb;
      existing.pssKb += e.pssKb;
      existing.sharedCleanKb += e.sharedCleanKb;
      existing.sharedDirtyKb += e.sharedDirtyKb;
      existing.privateCleanKb += e.privateCleanKb;
      existing.privateDirtyKb += e.privateDirtyKb;
      existing.swapKb += e.swapKb;
      existing.swapPssKb += e.swapPssKb;
      existing.entries.push(e);
    } else {
      groups.set(e.name, {
        name: e.name,
        count: 1,
        sizeKb: e.sizeKb,
        rssKb: e.rssKb,
        pssKb: e.pssKb,
        sharedCleanKb: e.sharedCleanKb,
        sharedDirtyKb: e.sharedDirtyKb,
        privateCleanKb: e.privateCleanKb,
        privateDirtyKb: e.privateDirtyKb,
        swapKb: e.swapKb,
        swapPssKb: e.swapPssKb,
        entries: [e],
      });
    }
  }
  const result = [...groups.values()];
  result.sort((a, b) => b.pssKb - a.pssKb);
  return result;
}

// ── Smaps rollup ────────────────────────────────────────────────────────────

export interface SmapsRollup {
  sizeKb: number;
  rssKb: number;
  pssKb: number;
  sharedCleanKb: number;
  sharedDirtyKb: number;
  privateCleanKb: number;
  privateDirtyKb: number;
  swapKb: number;
}

// ── OOM label mapping ────────────────────────────────────────────────────────

const OOM_LABEL_MAP: Record<string, string> = {
  'pers': 'Persistent',
  'top': 'Top',
  'bfgs': 'Bound FG Service',
  'btop': 'Bound Top',
  'fgs': 'FG Service',
  'fg': 'Foreground',
  'impfg': 'Important Foreground',
  'impbg': 'Important Background',
  'backup': 'Backup',
  'service': 'Service',
  'service-rs': 'Service Restarting',
  'receiver': 'Receiver',
  'heavy': 'Heavy Weight',
  'home': 'Home',
  'lastact': 'Last Activity',
  'cached': 'Cached',
  'cch': 'Cached',
  'frzn': 'Frozen',
  'native': 'Native',
  'sys': 'System',
  'fore': 'Foreground',
  'foreground': 'Foreground',
  'vis': 'Visible',
  'visible': 'Visible',
  'percep': 'Perceptible',
  'perceptible': 'Perceptible',
  'svcb': 'Service B',
  'svcrst': 'Service Restarting',
  'prev': 'Previous',
  'lstact': 'Last Activity',
};

function mapOomLabel(raw: string): string {
  const base = raw.replace(/\d+$/, '');
  return OOM_LABEL_MAP[base] ?? raw;
}

// ── LRU process list parsing ────────────────────────────────────────────────

// Matches: "  #0: fg     TOP  LCM 1234:com.android.systemui/u0a45 act:activities"
// Also:    "  #15: cch+75 CEM 9012:com.google.android.gms/u0a67"
const LRU_LINE = /^\s*#\d+:\s+(\S+)\s+.*?\s(\d+):([^\s/]+)/;

export const PINNED_PROCESSES = new Set([
  'system_server',
  'com.android.systemui',
]);

export function parseLruProcesses(output: string): ProcessInfo[] {
  const results: ProcessInfo[] = [];
  const seen = new Set<number>();
  for (const line of output.split('\n')) {
    const m = LRU_LINE.exec(line);
    if (!m) continue;
    const pid = parseInt(m[2], 10);
    if (!isFinite(pid) || seen.has(pid)) continue;
    seen.add(pid);
    const oomRaw = m[1].replace(/\+\d+$/, '');
    results.push({
      pid,
      name: m[3],
      oomLabel: mapOomLabel(oomRaw),
      pssKb: 0,
      rssKb: 0,
    });
  }
  return results;
}

// ── ADB sync pull ───────────────────────────────────────────────────────────

function encodeSyncCmd(cmd: string, length: number): Uint8Array {
  const buf = new Uint8Array(8);
  const dv = new DataView(buf.buffer);
  for (let i = 0; i < 4; i++) dv.setUint8(i, cmd.charCodeAt(i));
  dv.setUint32(4, length, true);
  return buf;
}

async function pullFile(
  device: AdbWebusbDevice,
  remotePath: string,
  onProgress?: (received: number, total: number) => void,
): Promise<Uint8Array> {
  // Get file size
  let fileSize = -1;
  try {
    const statResult = await shell(device, `stat -c %s '${remotePath}'`);
    const parsed = parseInt(statResult.trim(), 10);
    if (isFinite(parsed) && parsed > 0) fileSize = parsed;
  } catch {
    // stat failed
  }

  // Open sync stream
  const streamResult = await device.createStream('sync:');
  if (!streamResult.ok) throw new Error(streamResult.error);
  const stream = streamResult.value;

  return new Promise<Uint8Array>((resolve, reject) => {
    const chunks: Uint8Array[] = [];
    let received = 0;
    let headerBuf = new Uint8Array(0);

    stream.onData = (raw: Uint8Array) => {
      let buf: Uint8Array;
      if (headerBuf.length > 0) {
        buf = new Uint8Array(headerBuf.length + raw.length);
        buf.set(headerBuf, 0);
        buf.set(raw, headerBuf.length);
        headerBuf = new Uint8Array(0);
      } else {
        buf = raw;
      }

      let offset = 0;
      while (offset < buf.length) {
        if (buf.length - offset < 8) {
          headerBuf = buf.slice(offset);
          return;
        }
        const dv = new DataView(buf.buffer, buf.byteOffset + offset, 8);
        const cmd = String.fromCharCode(
          dv.getUint8(0),
          dv.getUint8(1),
          dv.getUint8(2),
          dv.getUint8(3),
        );
        const length = dv.getUint32(4, true);

        if (cmd === 'DATA') {
          offset += 8;
          const dataEnd = offset + length;
          if (dataEnd > buf.length) {
            headerBuf = buf.slice(offset - 8);
            return;
          }
          const chunk = buf.slice(offset, dataEnd);
          chunks.push(chunk);
          received += chunk.length;
          onProgress?.(received, fileSize);
          offset = dataEnd;
        } else if (cmd === 'DONE') {
          stream.close();
          const total = chunks.reduce((s, c) => s + c.length, 0);
          const result = new Uint8Array(total);
          let off = 0;
          for (const c of chunks) {
            result.set(c, off);
            off += c.length;
          }
          resolve(result);
          return;
        } else if (cmd === 'FAIL') {
          offset += 8;
          const msgEnd = offset + length;
          const decoder = new TextDecoder();
          const msg = decoder.decode(
            buf.subarray(offset, Math.min(msgEnd, buf.length)),
          );
          stream.close();
          reject(new Error(`ADB sync FAIL: ${msg}`));
          return;
        } else {
          stream.close();
          reject(new Error(`Unexpected sync response: ${cmd}`));
          return;
        }
      }
    };

    stream.onClose = () => {
      if (chunks.length > 0) {
        const total = chunks.reduce((s, c) => s + c.length, 0);
        const result = new Uint8Array(total);
        let off = 0;
        for (const c of chunks) {
          result.set(c, off);
          off += c.length;
        }
        resolve(result);
      } else {
        reject(new Error('Sync stream closed before receiving data'));
      }
    };

    // Send RECV command
    const pathBytes = utf8Encode(remotePath);
    const recvCmd = encodeSyncCmd('RECV', pathBytes.length);
    const sendBuf = new Uint8Array(recvCmd.length + pathBytes.length);
    sendBuf.set(recvCmd, 0);
    sendBuf.set(pathBytes, recvCmd.length);
    stream.write(sendBuf).catch(reject);
  });
}

// ── SmapsConnection ─────────────────────────────────────────────────────────

export class SmapsConnection {
  private device: AdbWebusbDevice | null = null;
  private keyMgr = new AdbKeyManager();
  private suPrefix = '';
  private _isRoot = false;

  get connected(): boolean {
    return this.device !== null;
  }
  get isRoot(): boolean {
    return this._isRoot;
  }

  async connect(onStatus?: (msg: string) => void): Promise<void> {
    if (navigator.usb === undefined) throw new Error('WebUSB not supported');
    const usbDev = await navigator.usb.requestDevice({
      filters: [ADB_DEVICE_FILTER],
    });
    onStatus?.('Authorize on device\u2026');
    const result = await AdbWebusbDevice.connect(usbDev, this.keyMgr);
    if (!result.ok) throw new Error(result.error);
    this.device = result.value;

    // Try to get root
    this._isRoot = false;
    this.suPrefix = '';
    for (const prefix of ['su 0', 'su -c']) {
      try {
        const out = await shell(this.device, `${prefix} id`);
        if (out.includes('uid=0')) {
          this._isRoot = true;
          this.suPrefix = prefix;
          break;
        }
      } catch {
        // Not rooted or wrong su variant
      }
    }
  }

  disconnect(): void {
    this.device?.close();
    this.device = null;
    this._isRoot = false;
    this.suPrefix = '';
  }

  async getProcessList(): Promise<ProcessInfo[]> {
    if (!this.device) throw new Error('Not connected');
    const output = await shell(this.device, 'dumpsys activity lru');
    const results = parseLruProcesses(output);
    // Add pinned system processes not already in the LRU list
    const seenPids = new Set(results.map((p) => p.pid));
    for (const name of PINNED_PROCESSES) {
      try {
        const pidStr = (await shell(this.device, `pidof ${name}`)).trim();
        const pid = parseInt(pidStr, 10);
        if (!isFinite(pid) || seenPids.has(pid)) continue;
        seenPids.add(pid);
        results.push({pid, name, oomLabel: 'System', pssKb: 0, rssKb: 0});
      } catch {
        // Process may not exist
      }
    }
    return results;
  }

  async getSmapsForPid(pid: number): Promise<SmapsEntry[]> {
    if (!this.device) throw new Error('Not connected');
    if (!this._isRoot) throw new Error('Root required');
    const cmd =
      this.suPrefix === 'su -c'
        ? `su -c 'cat /proc/${pid}/smaps'`
        : `su 0 cat /proc/${pid}/smaps`;
    const output = await shell(this.device, cmd);
    return parseSmaps(output);
  }

  async getSmapsRollupForPid(pid: number): Promise<SmapsRollup> {
    if (!this.device) throw new Error('Not connected');
    if (!this._isRoot) throw new Error('Root required');
    const cmd =
      this.suPrefix === 'su -c'
        ? `su -c 'cat /proc/${pid}/smaps_rollup'`
        : `su 0 cat /proc/${pid}/smaps_rollup`;
    const output = await shell(this.device, cmd);
    const r: SmapsRollup = {
      sizeKb: 0,
      rssKb: 0,
      pssKb: 0,
      sharedCleanKb: 0,
      sharedDirtyKb: 0,
      privateCleanKb: 0,
      privateDirtyKb: 0,
      swapKb: 0,
    };
    for (const line of output.split('\n')) {
      const match = /^(\w[\w_]*):\s+(\d+)\s+kB/.exec(line);
      if (match === null) continue;
      const val = parseInt(match[2], 10);
      switch (match[1]) {
        case 'Rss':
          r.rssKb = val;
          break;
        case 'Pss':
          r.pssKb = val;
          break;
        case 'Shared_Clean':
          r.sharedCleanKb = val;
          break;
        case 'Shared_Dirty':
          r.sharedDirtyKb = val;
          break;
        case 'Private_Clean':
          r.privateCleanKb = val;
          break;
        case 'Private_Dirty':
          r.privateDirtyKb = val;
          break;
        case 'Swap':
          r.swapKb = val;
          break;
      }
    }
    return r;
  }

  async enrichProcesses(
    processes: ProcessInfo[],
    onProgress?: (done: number, total: number) => void,
  ): Promise<Map<number, SmapsRollup>> {
    if (!this.device || !this._isRoot) return new Map();
    const rollups = new Map<number, SmapsRollup>();
    for (let i = 0; i < processes.length; i++) {
      try {
        const r = await this.getSmapsRollupForPid(processes[i].pid);
        rollups.set(processes[i].pid, r);
      } catch {
        // Process may have died
      }
      onProgress?.(i + 1, processes.length);
    }
    return rollups;
  }

  async dumpVmaMemory(
    pid: number,
    regions: {addrStart: string; addrEnd: string}[],
    onProgress: (status: string) => void,
  ): Promise<Uint8Array> {
    if (!this.device) throw new Error('Not connected');
    if (!this._isRoot) throw new Error('Root required');
    if (regions.length === 0) throw new Error('No regions');

    const tmpPath = `/data/local/tmp/vma_${pid}_${Date.now()}.bin`;
    const ddCmds = regions.map((r, i) => {
      const startByte = parseInt(r.addrStart, 16);
      const endByte = parseInt(r.addrEnd, 16);
      const startPage = Math.floor(startByte / 4096);
      const numPages = Math.ceil((endByte - startByte) / 4096);
      const redir = i === 0 ? '>' : '>>';
      return `dd if=/proc/${pid}/mem bs=4096 skip=${startPage} count=${numPages} ${redir} ${tmpPath} 2>/dev/null`;
    });

    try {
      onProgress('Reading memory\u2026');
      const shellCmd =
        this.suPrefix === 'su -c'
          ? `su -c '${ddCmds.join(' && ')}'`
          : `su 0 sh -c '${ddCmds.join(' && ')}'`;
      await shell(this.device, shellCmd);

      onProgress('Pulling\u2026');
      const data = await pullFile(this.device, tmpPath, (received, total) => {
        const mb = (received / 1_048_576).toFixed(1);
        const pct = total > 0 ? Math.round((100 * received) / total) : 0;
        onProgress(`Pulling: ${mb} MiB (${pct}%)`);
      });
      return data;
    } finally {
      try {
        await shell(this.device, `rm -f ${tmpPath}`);
      } catch {
        // ignore
      }
    }
  }

  async captureHeapDump(
    pid: number,
    onProgress: (status: string) => void,
  ): Promise<Uint8Array> {
    if (!this.device) throw new Error('Not connected');
    const tmpPath = `/data/local/tmp/heap_${pid}_${Date.now()}.hprof`;
    try {
      onProgress('Dumping heap\u2026');
      await shell(this.device, `am dumpheap ${pid} ${tmpPath}`);
      // Wait for dump to complete
      await new Promise((resolve) => setTimeout(resolve, 2000));
      onProgress('Pulling\u2026');
      const data = await pullFile(this.device, tmpPath, (received, total) => {
        const mb = (received / 1_048_576).toFixed(1);
        const pct = total > 0 ? Math.round((100 * received) / total) : 0;
        onProgress(`Pulling heap: ${mb} MiB (${pct}%)`);
      });
      return data;
    } finally {
      try {
        await shell(this.device, `rm -f ${tmpPath}`);
      } catch {
        // ignore
      }
    }
  }

  async grepVmaStrings(
    pid: number,
    entries: SmapsEntry[],
    onBatch: (
      newStrings: VmaString[],
      regions: VmaRegionInfo[],
      completed: number,
      total: number,
    ) => void,
  ): Promise<{regions: VmaRegionInfo[]; strings: VmaString[]}> {
    if (!this.device) throw new Error('Not connected');
    if (!this._isRoot) throw new Error('Root required');

    const BATCH_SIZE = 8;
    const MARKER = '___AHAT_VMA_BOUNDARY___';
    const readable = entries.filter((e) => e.perms[0] === 'r');
    const regions: VmaRegionInfo[] = readable.map((e) => ({
      addrStart: e.addrStart,
      addrEnd: e.addrEnd,
      perms: e.perms,
      name: e.name,
      sizeKb: e.sizeKb,
      stringCount: 0,
    }));
    const strings: VmaString[] = [];

    for (
      let batchStart = 0;
      batchStart < readable.length;
      batchStart += BATCH_SIZE
    ) {
      const batchEnd = Math.min(batchStart + BATCH_SIZE, readable.length);
      const cmds: string[] = [];
      const batchMeta: {index: number; startByte: number}[] = [];
      for (let i = batchStart; i < batchEnd; i++) {
        const e = readable[i];
        const startByte = parseInt(e.addrStart, 16);
        const endByte = parseInt(e.addrEnd, 16);
        const startPage = Math.floor(startByte / 4096);
        const numPages = Math.ceil((endByte - startByte) / 4096);
        batchMeta.push({index: i, startByte});
        cmds.push(`echo ${MARKER}`);
        cmds.push(
          `dd if=/proc/${pid}/mem bs=4096 skip=${startPage} count=${numPages} 2>/dev/null | grep -baoE "[ -~]{4,}"`,
        );
      }

      try {
        const innerCmd = cmds.join(';');
        const shellCmd =
          this.suPrefix === 'su -c'
            ? `su -c '${innerCmd}'`
            : `su 0 sh -c '${innerCmd}'`;
        const output = await shell(this.device, shellCmd);
        const sections = output.split(MARKER);
        const batchStrings: VmaString[] = [];
        for (let si = 0; si < batchMeta.length; si++) {
          const section = sections[si + 1] ?? '';
          const {index, startByte} = batchMeta[si];
          const parsed = parseGrepOutput(section);
          regions[index].stringCount = parsed.length;
          for (const p of parsed) {
            const vs: VmaString = {
              offset: p.offset,
              vmaAddr: startByte + p.offset,
              str: p.str,
              vmaIndex: index,
            };
            strings.push(vs);
            batchStrings.push(vs);
          }
        }
        onBatch(batchStrings, regions, batchEnd, readable.length);
      } catch {
        onBatch([], regions, batchEnd, readable.length);
      }
    }

    return {regions, strings};
  }
}
