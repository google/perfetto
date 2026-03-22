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
import {Anchor} from '../../widgets/anchor';
import {Spinner} from '../../widgets/spinner';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import {type SchemaRegistry} from '../../components/widgets/datagrid/datagrid_schema';
import {type Row, type SqlValue} from '../../trace_processor/query_result';
import {type TabsTab} from '../../widgets/tabs';
import {type ProcessStringsResult, type VmaString} from './smaps_connection';
import {
  hexAddrRenderer,
  computeDuplicates,
  ALL_STRINGS_SCHEMA,
  type DuplicateGroup,
} from './data';
import {
  TAB_STRINGS_ALL,
  TAB_STRINGS_DUPS,
  TAB_STRINGS_VMA,
  type StringsState,
} from './state';

// ── Duplicate computation cache ─────────────────────────────────────────────

export function getDupsFor(
  ss: StringsState,
  strings: VmaString[],
): DuplicateGroup[] {
  if (strings !== ss.cachedDupsStrings) {
    ss.cachedDupsStrings = strings;
    ss.cachedDups = computeDuplicates(strings);
  }
  return ss.cachedDups;
}

// Sets a filter on the All Strings tab and switches to it.
function filterAndSwitch(
  ss: StringsState,
  switchToStrings: () => void,
  field: string,
  value: string,
): void {
  ss.stringsInitialFilters = [{field, op: '=', value}];
  ss.stringsFilterKey++;
  switchToStrings();
}

// ── All Strings tab ─────────────────────────────────────────────────────────

export function renderAllStrings(
  ss: StringsState,
  strings: VmaString[],
  data: ProcessStringsResult,
): m.Children {
  if (data.scanning && strings.length === 0) {
    return m('.pf-smaps-explorer__loading', m(Spinner));
  }

  const rows: Row[] = strings.map((s) => ({
    vmaAddr: s.vmaAddr,
    vmaName: data.regions[s.vmaIndex]?.name ?? '',
    str: s.str,
  }));

  return m(DataGrid, {
    key: ss.stringsFilterKey,
    schema: ALL_STRINGS_SCHEMA,
    rootSchema: 'string',
    data: rows,
    fillHeight: true,
    initialColumns: [
      {id: 'vmaAddr', field: 'vmaAddr', sort: 'ASC' as const},
      {id: 'vmaName', field: 'vmaName'},
      {id: 'str', field: 'str'},
    ],
    initialFilters:
      ss.stringsInitialFilters.length > 0
        ? ss.stringsInitialFilters
        : undefined,
  });
}

// ── Duplicates tab ──────────────────────────────────────────────────────────

function buildDuplicatesSchema(
  ss: StringsState,
  switchToStrings: () => void,
): SchemaRegistry {
  return {
    duplicate: {
      totalBytes: {title: 'Bytes', columnType: 'quantitative'},
      count: {title: 'Count', columnType: 'quantitative'},
      length: {title: 'Len', columnType: 'quantitative'},
      vmaCount: {title: 'VMAs', columnType: 'quantitative'},
      value: {
        title: 'String',
        columnType: 'text',
        cellRenderer: (value: SqlValue) => {
          const str = String(value);
          return m(
            Anchor,
            {
              onclick: () => filterAndSwitch(ss, switchToStrings, 'str', str),
              title: 'Filter strings tab by this value',
            },
            str,
          );
        },
      },
    },
  };
}

export function renderDuplicates(
  ss: StringsState,
  switchToStrings: () => void,
  dups: DuplicateGroup[],
  scanning: boolean,
): m.Children {
  if (scanning && dups.length === 0) {
    return m('.pf-smaps-explorer__loading', m(Spinner));
  }

  const rows: Row[] = dups.map((d) => ({
    totalBytes: d.totalBytes,
    count: d.count,
    length: d.value.length,
    vmaCount: d.vmaCount,
    value: d.value,
  }));

  return m(DataGrid, {
    schema: buildDuplicatesSchema(ss, switchToStrings),
    rootSchema: 'duplicate',
    data: rows,
    fillHeight: true,
    initialColumns: [
      {id: 'totalBytes', field: 'totalBytes', sort: 'DESC' as const},
      {id: 'count', field: 'count'},
      {id: 'length', field: 'length'},
      {id: 'vmaCount', field: 'vmaCount'},
      {id: 'value', field: 'value'},
    ],
  });
}

// ── By VMA tab ──────────────────────────────────────────────────────────────

function buildByVmaSchema(
  ss: StringsState,
  switchToStrings: () => void,
): SchemaRegistry {
  return {
    vma: {
      addrStartNum: {
        title: 'Address',
        columnType: 'quantitative',
        cellRenderer: hexAddrRenderer,
      },
      perms: {title: 'Perms', columnType: 'text'},
      name: {
        title: 'Name',
        columnType: 'text',
        cellRenderer: (value: SqlValue) => {
          const name = String(value) || '[anonymous]';
          return m(
            Anchor,
            {
              onclick: () =>
                filterAndSwitch(ss, switchToStrings, 'vmaName', name),
              title: 'Filter strings tab to this VMA',
            },
            name,
          );
        },
      },
      stringCount: {title: 'Strings', columnType: 'quantitative'},
      sizeKb: {title: 'Size', columnType: 'quantitative'},
    },
  };
}

export function renderByVma(
  ss: StringsState,
  switchToStrings: () => void,
  strings: VmaString[],
  data: ProcessStringsResult,
): m.Children {
  if (data.scanning && strings.length === 0) {
    return m('.pf-smaps-explorer__loading', m(Spinner));
  }

  const vmaCounts = new Map<number, number>();
  for (const s of strings) {
    vmaCounts.set(s.vmaIndex, (vmaCounts.get(s.vmaIndex) ?? 0) + 1);
  }

  const rows: Row[] = data.regions
    .map((r, i) => ({
      addrStartNum: parseInt(r.addrStart, 16),
      perms: r.perms,
      name: r.name,
      stringCount: vmaCounts.get(i) ?? 0,
      sizeKb: r.sizeKb,
    }))
    .filter((r) => r.stringCount > 0);

  return m(DataGrid, {
    schema: buildByVmaSchema(ss, switchToStrings),
    rootSchema: 'vma',
    data: rows,
    fillHeight: true,
    initialColumns: [
      {id: 'addrStartNum', field: 'addrStartNum'},
      {id: 'perms', field: 'perms'},
      {id: 'name', field: 'name'},
      {id: 'stringCount', field: 'stringCount', sort: 'DESC' as const},
      {id: 'sizeKb', field: 'sizeKb'},
    ],
  });
}

// ── Tab builders ────────────────────────────────────────────────────────────

/**
 * Build the strings-related TabsTab entries (All Strings, Duplicates, By VMA)
 * for a given StringsState.
 *
 * @param titlePrefix - prefix like 'All ' for process-level strings
 * @param contextLabel - bracketed context, e.g. '[7f00-7f10 libc.so]'
 */
export function buildStringsTabs(
  ss: StringsState,
  dups: DuplicateGroup[],
  switchToStringsTab: () => void,
  titlePrefix: string,
  contextLabel: string,
): TabsTab[] {
  const sd = ss.stringsData!;
  const strings = sd.strings;
  const scanning = sd.scanning === true;
  const progress = scanning
    ? ` (${sd.scannedVmas ?? 0}/${sd.totalVmas ?? 0})`
    : '';
  const ctx = contextLabel !== '' ? ` [${contextLabel}]` : '';

  const tabs: TabsTab[] = [
    {
      key: TAB_STRINGS_ALL,
      title: `${titlePrefix}Strings${progress}${ctx}`,
      closeButton: true,
      content: renderAllStrings(ss, strings, sd),
    },
    {
      key: TAB_STRINGS_DUPS,
      title: `${titlePrefix}Duplicates (${dups.length})${ctx}`,
      closeButton: true,
      content: renderDuplicates(ss, switchToStringsTab, dups, scanning),
    },
  ];
  if (sd.regions.length > 1) {
    tabs.push({
      key: TAB_STRINGS_VMA,
      title: `${titlePrefix}Strings by VMA${ctx}`,
      closeButton: true,
      content: renderByVma(ss, switchToStringsTab, strings, sd),
    });
  }
  return tabs;
}

/** Handle closing a strings tab: clear data and reset subTab if needed. */
export function closeStringsTab(
  ss: StringsState,
  key: string,
  currentSubTab: string,
  defaultTab: string,
): string {
  ss.stringsData = null;
  ss.stringsInitialFilters = [];
  ss.stringsFilterKey = 0;
  return key === currentSubTab ? defaultTab : currentSubTab;
}
