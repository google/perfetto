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

import {type Filter} from '../../components/widgets/datagrid/model';
import {type App} from '../../public/app';
import {
  SmapsConnection,
  type ProcessInfo,
  type SmapsAggregated,
  type SmapsRollup,
  type ProcessStringsResult,
  type VmaString,
} from './smaps_connection';
import {type DuplicateGroup, type VmaFilters} from './data';

// ── Tab keys ────────────────────────────────────────────────────────────────

export const TAB_PROCESSES = 'processes';
export const TAB_VMAS = 'vmas';
export const TAB_INSPECT = 'inspect';
export const TAB_MAPPING = 'mapping';
export const TAB_STRINGS_ALL = 'strings_all';
export const TAB_STRINGS_DUPS = 'strings_dups';
export const TAB_STRINGS_VMA = 'strings_vma';

// Prefixed tab key helpers — centralises encoding/decoding of composite keys
// like `proc_123` or `map_libc.so` that appear throughout the tab system.
const PROC_PREFIX = 'proc_';
const MAP_PREFIX = 'map_';
const VMAP_PREFIX = 'vmap_';

export function procTabKey(pid: number): string {
  return `${PROC_PREFIX}${pid}`;
}
export function mapTabKey(name: string): string {
  return `${MAP_PREFIX}${name}`;
}
export function vmapTabKey(name: string): string {
  return `${VMAP_PREFIX}${name}`;
}
export function parseProcTabKey(key: string): number | undefined {
  return key.startsWith(PROC_PREFIX)
    ? parseInt(key.slice(PROC_PREFIX.length), 10)
    : undefined;
}
export function parseMapTabKey(key: string): string | undefined {
  return key.startsWith(MAP_PREFIX) ? key.slice(MAP_PREFIX.length) : undefined;
}
export function parseVmapTabKey(key: string): string | undefined {
  return key.startsWith(VMAP_PREFIX)
    ? key.slice(VMAP_PREFIX.length)
    : undefined;
}

// ── Strings state (shared between mapping-level and process-level) ────────

export interface StringsState {
  stringsData: ProcessStringsResult | null;
  stringsFilterKey: number;
  stringsInitialFilters: readonly Filter[];
  cachedDups: DuplicateGroup[];
  cachedDupsStrings: VmaString[] | null;
}

// ── Per-mapping tab state ─────────────────────────────────────────────────

export interface MappingTabState extends StringsState {
  subTab: string;
}

export function newMappingTabState(): MappingTabState {
  return {
    subTab: TAB_MAPPING,
    stringsData: null,
    stringsFilterKey: 0,
    stringsInitialFilters: [],
    cachedDups: [],
    cachedDupsStrings: null,
  };
}

// ── Per-process tab state ──────────────────────────────────────────────────

export interface ProcessTabState {
  subTab: string;
  openMappings: Map<string, MappingTabState>;
  openMappingOrder: string[];
  activeMapping: string | null;
  processStringsData: ProcessStringsResult | null;
  processStringsDups: DuplicateGroup[];
  processStringsDupsStrings: VmaString[] | null;
  processStringsFilterKey: number;
  processStringsInitialFilters: readonly Filter[];
}

export function newProcessTabState(): ProcessTabState {
  return {
    subTab: TAB_INSPECT,
    openMappings: new Map(),
    openMappingOrder: [],
    activeMapping: null,
    processStringsData: null,
    processStringsDups: [],
    processStringsDupsStrings: null,
    processStringsFilterKey: 0,
    processStringsInitialFilters: [],
  };
}

// ── Per-VMA-mapping state (VMA View) ──────────────────────────────────────

export interface VmaMappingTabState {
  subTab: string;
  openProcs: Map<number, MappingTabState>;
  openProcOrder: number[];
  activeProc: number | null;
}

export function newVmaMappingTabState(): VmaMappingTabState {
  return {
    subTab: 'procs',
    openProcs: new Map(),
    openProcOrder: [],
    activeProc: null,
  };
}

// ── Page context (passed to extracted view modules) ─────────────────────────

export interface PageContext {
  // Data
  readonly processes: ProcessInfo[] | null;
  readonly smapsData: Map<number, SmapsAggregated[]>;
  readonly rollups: Map<number, SmapsRollup>;
  readonly vmaFilters: VmaFilters;
  readonly isRoot: boolean;

  // UI state
  readonly loadingPid: number | null;
  readonly enrichGeneration: number;
  readonly smapsScanGeneration: number;
  readonly scanningAllSmaps: boolean;

  // Tab state (mutable by views)
  readonly s: SmapsStore;

  // Actions
  inspectProcess(pid: number): void;
  openMapping(ps: ProcessTabState, name: string): void;
  openVmaProcesses(name: string): void;
  openVmaProcDetail(vs: VmaMappingTabState, pid: number): void;
  scanSingleVma(
    pid: number,
    ms: MappingTabState,
    addrStart: string,
    addrEnd: string,
    perms: string,
  ): Promise<void>;
  startStringsScan(
    pid: number,
    processName: string,
    ps: ProcessTabState,
  ): Promise<void>;
  captureHeap(pid: number, name: string, app: App): Promise<void>;
  scanAllSmaps(): Promise<void>;
  setVmaFilters(f: VmaFilters): void;
  getProcessStringsState(ps: ProcessTabState): StringsState;
}

// ── Tab close helper (shared by process_view and vma_view) ──────────────────

/**
 * Close a tab from an ordered map of open items.
 * Returns the key to activate if the closed item was currently active.
 */
export function closeTab<K>(
  items: Map<K, unknown>,
  order: K[],
  activeKey: K | null,
  closedKey: K,
  defaultKey: K | null,
): K | null {
  items.delete(closedKey);
  const idx = order.indexOf(closedKey);
  if (idx >= 0) order.splice(idx, 1);
  if (activeKey !== closedKey) return activeKey;
  if (order.length > 0) {
    return idx > 0 ? order[idx - 1] : order[0];
  }
  return defaultKey;
}

// ── Persistent store (survives page navigation) ────────────────────────────

export interface SmapsStore {
  conn: SmapsConnection;
  processes: ProcessInfo[] | null;
  rollups: Map<number, SmapsRollup>;
  smapsData: Map<number, SmapsAggregated[]>;
  openProcesses: Map<number, ProcessTabState>;
  openProcessOrder: number[];
  activeProcessPid: number | null;
  openVmaMappings: Map<string, VmaMappingTabState>;
  openVmaMappingOrder: string[];
  activeVmaMapping: string | null;
  topView: 0 | 1;
  processTab: string;
  vmaTab: string;
  vmaFilters: VmaFilters;
}

let store: SmapsStore | undefined;

export function getStore(): SmapsStore {
  if (store === undefined) {
    store = {
      conn: new SmapsConnection(),
      processes: null,
      rollups: new Map(),
      smapsData: new Map(),
      openProcesses: new Map(),
      openProcessOrder: [],
      activeProcessPid: null,
      openVmaMappings: new Map(),
      openVmaMappingOrder: [],
      activeVmaMapping: null,
      topView: 0,
      processTab: TAB_PROCESSES,
      vmaTab: TAB_VMAS,
      vmaFilters: {type: 'all', r: null, w: null, x: null},
    };
  }
  return store;
}
