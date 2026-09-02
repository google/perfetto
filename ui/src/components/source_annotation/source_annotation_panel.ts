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
import {AsyncMemo} from '../../base/async_memo';
import {classNames} from '../../base/classnames';
import type {Trace} from '../../public/trace';
import {DetailsShell} from '../../widgets/details_shell';
import {EmptyState} from '../../widgets/empty_state';
import {Grid, GridCell, GridHeaderCell, type GridRow} from '../../widgets/grid';
import {Spinner} from '../../widgets/spinner';
import {Tabs} from '../../widgets/tabs';
import type {TreeExplorerOptionalAction} from '../../widgets/tree_explorer';
import type {AggTreeExplorerQueryColumn} from '../tree_explorer_fetcher';
import {
  highlightInstruction,
  highlightSourceLine,
  languageForPath,
} from './highlight';
import {
  type JumpArcLayout,
  jumpGutterWidthPx,
  layoutJumpArcs,
  renderJumpGutterRow,
} from './jump_arrows';
import {
  loadSourceAnnotation,
  type AnnotatedInstruction,
  type AnnotatedLine,
  type SourceAnnotation,
  type SourceAnnotationTarget,
} from './source_annotation_loader';

import './source_annotation_panel.scss';

const ROW_HEIGHT_PX = 24;
const TAB_SOURCE = 'source';
const TAB_ASSEMBLY = 'assembly';

export interface SourceAnnotationPanelAttrs {
  readonly trace: Trace;
  readonly title: string;
  readonly target: SourceAnnotationTarget;
}

// Shows the source and disassembly of a function with the number of samples
// on each line and instruction.
export class SourceAnnotationPanel implements m.ClassComponent<SourceAnnotationPanelAttrs> {
  private readonly memo = new AsyncMemo<SourceAnnotation>();
  private activeTab?: string;

  view({attrs}: m.CVnode<SourceAnnotationPanelAttrs>): m.Children {
    const {target} = attrs;
    const result = this.memo.use({
      key: {
        functionName: target.functionName,
        mappingName: target.mappingName,
        mappingId: target.mappingId,
        relPc: target.relPc?.toString(),
        sourceFile: target.sourceFile,
        samplesSql: target.samplesSql,
      },
      compute: () => loadSourceAnnotation(attrs.trace.engine, target),
    });
    const data = result.data;
    return m(
      DetailsShell,
      {
        fillHeight: true,
        title: attrs.title,
        description: data?.sourceFile,
      },
      data === undefined ? m(Spinner, {easing: true}) : this.renderTabs(data),
    );
  }

  private renderTabs(data: SourceAnnotation): m.Children {
    const tabs = [];
    if (data.lines.length > 0) {
      tabs.push({
        key: TAB_SOURCE,
        title: 'Source',
        leftIcon: 'code',
        content: renderSource(data),
      });
    }
    if (data.instructions.length > 0) {
      tabs.push({
        key: TAB_ASSEMBLY,
        title: 'Assembly',
        leftIcon: 'memory',
        content: renderAssembly(data),
      });
    }
    if (tabs.length === 0) {
      return m(EmptyState, {
        icon: 'code_off',
        title: 'No source or disassembly bundled for this function',
        description:
          'Run `trace_processor bundle` with --symbol-paths pointing at the ' +
          'unstripped binaries to bundle them with the trace.',
      });
    }
    const activeTabKey =
      this.activeTab !== undefined && tabs.some((t) => t.key === this.activeTab)
        ? this.activeTab
        : tabs[0].key;
    return m(Tabs, {
      className: 'pf-source-annotation',
      tabs,
      activeTabKey,
      onTabChange: (key) => {
        this.activeTab = key;
      },
    });
  }
}

function renderSource(data: SourceAnnotation): m.Children {
  return m(Grid, {
    className: 'pf-source-annotation__grid',
    columns: [
      {key: 'self', header: m(GridHeaderCell, 'Self')},
      {key: 'total', header: m(GridHeaderCell, 'Total')},
      {key: 'line', header: m(GridHeaderCell, 'Line')},
      {
        key: 'source',
        maxInitialWidthPx: Infinity,
        header: m(GridHeaderCell, 'Source'),
      },
    ],
    rowData: highlightedSourceRows(data),
    virtualization: {rowHeightPx: ROW_HEIGHT_PX},
    fillHeight: true,
  });
}

// Block comments span lines, so the lines are tokenized in order.
function highlightedSourceRows(data: SourceAnnotation): GridRow[] {
  const language = languageForPath(data.sourceFile);
  let inBlockComment = false;
  return data.lines.map((line) => {
    const highlighted = highlightSourceLine(
      line.text,
      language,
      inBlockComment,
    );
    inBlockComment = highlighted.inBlockComment;
    return sourceRow(line, highlighted.children, data);
  });
}

function sourceRow(
  line: AnnotatedLine,
  code: m.Children,
  data: SourceAnnotation,
): GridRow {
  return [
    countCell(line.selfCount, data.maxLineSelf),
    countCell(line.totalCount, data.maxLineTotal),
    m(
      GridCell,
      {align: 'right', className: 'pf-source-annotation__line-number'},
      line.lineNumber,
    ),
    m(GridCell, m('span.pf-source-annotation__code', code)),
  ];
}

function renderAssembly(data: SourceAnnotation): m.Children {
  const jumps = layoutJumpArcs(data.instructions);
  const gutterWidth = jumpGutterWidthPx(jumps);
  return m(Grid, {
    className: 'pf-source-annotation__grid',
    columns: [
      {key: 'self', header: m(GridHeaderCell, 'Self')},
      {key: 'total', header: m(GridHeaderCell, 'Total')},
      {key: 'address', header: m(GridHeaderCell, 'Address')},
      {key: 'line', header: m(GridHeaderCell, 'Line')},
      {key: 'bytes', header: m(GridHeaderCell, 'Bytes')},
      // Branch arrows to targets within the function.
      ...(gutterWidth > 0
        ? [{key: 'jumps', widthPx: gutterWidth, header: m(GridHeaderCell)}]
        : []),
      {
        key: 'instruction',
        maxInitialWidthPx: Infinity,
        header: m(GridHeaderCell, 'Instruction'),
      },
    ],
    rowData: data.instructions.map((insn, index) =>
      instructionRow(insn, index, jumps, data),
    ),
    virtualization: {rowHeightPx: ROW_HEIGHT_PX},
    fillHeight: true,
  });
}

function instructionRow(
  insn: AnnotatedInstruction,
  index: number,
  jumps: JumpArcLayout,
  data: SourceAnnotation,
): GridRow {
  return [
    countCell(insn.selfCount, data.maxInstructionSelf),
    countCell(insn.totalCount, data.maxInstructionTotal),
    m(
      GridCell,
      {className: 'pf-source-annotation__address'},
      `0x${insn.relPc.toString(16)}`,
    ),
    m(
      GridCell,
      {align: 'right', className: 'pf-source-annotation__line-number'},
      insn.lineNumber,
    ),
    m(GridCell, {className: 'pf-source-annotation__bytes'}, insn.bytes),
    ...(jumps.laneCount > 0
      ? [
          m(
            GridCell,
            {padding: false, className: 'pf-source-annotation__jumps'},
            renderJumpGutterRow(index, jumps, ROW_HEIGHT_PX),
          ),
        ]
      : []),
    m(
      GridCell,
      m('span.pf-source-annotation__code', highlightInstruction(insn.text)),
    ),
  ];
}

// A count with a background shade proportional to its share of the hottest
// row, so hot spots stand out at a glance.
function countCell(count: number, max: number): m.Children {
  const level = count <= 0 || max <= 0 ? 0 : Math.ceil((4 * count) / max);
  return m(
    GridCell,
    {
      align: 'right',
      className: classNames(
        'pf-source-annotation__count',
        level > 0 && `pf-source-annotation__count--heat-${level}`,
      ),
    },
    count > 0 ? count : '',
  );
}

// Opens a SourceAnnotationPanel in an ephemeral tab keyed by the target, so
// re-invoking for the same function reuses the tab.
export function openSourceAnnotationTab(
  trace: Trace,
  title: string,
  target: SourceAnnotationTarget,
): void {
  const uri = `source_annotation#${target.mappingName ?? ''}/${target.functionName}/${title}`;
  trace.tabs.registerTab({
    uri,
    isEphemeral: true,
    content: {
      getTitle: () => title,
      render: () => m(SourceAnnotationPanel, {trace, title, target}),
    },
  });
  trace.tabs.showTab(uri);
}

// Hidden properties identifying the function of a callstack tree node, for
// `sourceAnnotationNodeAction`. Metrics using it must select the columns
// `source_file`, `rel_pc` and `mapping_id` which the `_callstacks_for_*`
// macros of `callstacks.stack_profile` provide.
export const SOURCE_ANNOTATION_PROPERTIES: ReadonlyArray<AggTreeExplorerQueryColumn> =
  [
    {
      name: 'source_file',
      displayName: 'Source File',
      mergeAggregation: 'ONE_OR_SUMMARY',
      isVisible: () => false,
    },
    {
      name: 'rel_pc',
      displayName: 'Address',
      mergeAggregation: 'ONE_OR_SUMMARY',
      isVisible: () => false,
    },
    {
      name: 'mapping_id',
      displayName: 'Mapping Id',
      mergeAggregation: 'ONE_OR_SUMMARY',
      isVisible: () => false,
    },
  ];

// A node action opening the annotated source and disassembly of the node's
// function, counting the samples in `samplesSql` (see
// SourceAnnotationTarget).
export function sourceAnnotationNodeAction(
  trace: Trace,
  samplesSql: string,
): TreeExplorerOptionalAction {
  return {
    name: 'View source & assembly',
    icon: 'code',
    category: 'DRILL',
    description:
      'Show the source and disassembly of this function with the number ' +
      'of samples on each line and instruction.',
    execute: ({node, properties}) => {
      if (node === undefined) return;
      openSourceAnnotationTab(
        trace,
        node.name,
        sourceAnnotationTargetFromProperties(node.name, properties, samplesSql),
      );
    },
  };
}

// Builds the target for a callstack tree node from the node's properties.
// Aggregated properties of merged nodes read `<value> and N others`; the
// first value is representative enough to locate the function.
export function sourceAnnotationTargetFromProperties(
  functionName: string,
  properties: ReadonlyMap<string, string>,
  samplesSql: string,
): SourceAnnotationTarget {
  const first = (key: string): string | undefined => {
    const value = properties.get(key)?.split(' and ')[0];
    // Symbolizers report an unknown file as '??'.
    if (value === undefined || value === '' || value === '??') {
      return undefined;
    }
    return value;
  };
  const mappingId = first('mapping_id');
  const relPc = first('rel_pc');
  return {
    functionName,
    mappingName: first('mapping_name'),
    mappingId: mappingId !== undefined ? Number(mappingId) : undefined,
    relPc: relPc !== undefined ? BigInt(relPc) : undefined,
    sourceFile: first('source_file'),
    samplesSql,
  };
}
