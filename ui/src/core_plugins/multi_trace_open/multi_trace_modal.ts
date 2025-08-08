// Copyright (C) 2023 The Android Open Source Project
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
import {AppImpl} from '../../core/app_impl';
import {Button, ButtonVariant} from '../../widgets/button';
import {CardStack} from '../../widgets/card';
import {Intent} from '../../widgets/common';
import {Icon} from '../../widgets/icon';
import {closeModal, redrawModal, showModal} from '../../widgets/modal';
import {Select} from '../../widgets/select';
import {Callout} from '../../widgets/callout';
import {Spinner} from '../../widgets/spinner';
import {Stack} from '../../widgets/stack';
import {Switch} from '../../widgets/switch';
import {TextInput} from '../../widgets/text_input';
import {MultiTraceController} from './multi_trace_controller';
import {
  AnchorLink,
  SyncConfig,
  AnchoredConfig,
  TraceFile,
  TraceFileAnalyzed,
  TraceStatus,
} from './multi_trace_types';
import {WasmTraceAnalyzer} from './trace_analyzer';

const MODAL_KEY = 'multi-trace-modal';

// =============================================================================
// Shell Component
// =============================================================================

interface MultiTraceModalAttrs {
  initialFiles: ReadonlyArray<File>;
}

class MultiTraceModalShell implements m.ClassComponent<MultiTraceModalAttrs> {
  private controller = new MultiTraceController(new WasmTraceAnalyzer(), () =>
    redrawModal(),
  );

  oncreate({attrs}: m.Vnode<MultiTraceModalAttrs>) {
    this.controller.addFiles(attrs.initialFiles);
  }

  view() {
    return m(
      Stack,
      {className: 'pf-multi-trace-modal', orientation: 'vertical'},
      m(
        Stack,
        {
          className: 'pf-multi-trace-modal__main',
          orientation: 'horizontal',
        },
        m(TraceListComponent, {
          traces: this.controller.traces,
          controller: this.controller,
        }),
        this.controller.selectedTrace && [
          m('.pf-multi-trace-modal__separator'),
          m(TraceDetailsPanelComponent, {
            trace: this.controller.selectedTrace,
            controller: this.controller,
          }),
        ],
      ),
      m(
        Stack,
        {className: 'pf-multi-trace-modal__footer', orientation: 'horizontal'},
        this.renderActions(),
      ),
    );
  }

  private renderActions() {
    const footerMessage = this.getFooterMessage();
    const isDisabled = !!footerMessage;
    const openButton = m(Button, {
      label: 'Open Traces',
      intent: Intent.Primary,
      variant: ButtonVariant.Filled,
      onclick: () => this.openTraces(),
      disabled: isDisabled,
    });

    return [
      footerMessage &&
        m(
          Callout,
          {
            className: 'pf-multi-trace-modal__footer-error',
            intent: Intent.Danger,
            icon: 'error_outline',
          },
          footerMessage,
        ),
      openButton,
    ];
  }

  private getFooterMessage(): string | undefined {
    const error = this.controller.getLoadingError();
    if (error === undefined) {
      return undefined;
    }
    switch (error) {
      case 'NO_TRACES':
        return 'Add at least one trace to open.';
      case 'ANALYZING':
        return 'Wait for all traces to be analyzed and synced.';
      case 'SYNC_ERROR':
        return this.controller.syncError;
      case 'TRACE_ERROR':
        return 'Remove traces with errors before opening.';
      case 'INCOMPLETE_CONFIG':
        return 'All traces must be fully configured before opening.';
      default:
        return 'An unknown error occurred.';
    }
  }

  private openTraces() {
    if (this.controller.traces.length === 0) {
      return;
    }
    const files = this.controller.traces.map((t) => t.file);
    AppImpl.instance.openTraceFromMultipleFiles(files);
    closeModal(MODAL_KEY);
  }
}

// =============================================================================
// Trace List Component
// =============================================================================

interface TraceListComponentAttrs {
  traces: ReadonlyArray<TraceFile>;
  controller: MultiTraceController;
}

class TraceListComponent implements m.ClassComponent<TraceListComponentAttrs> {
  view({attrs}: m.Vnode<TraceListComponentAttrs>) {
    const {traces, controller} = attrs;
    return m(
      Stack,
      {className: 'pf-multi-trace-modal__list-panel', orientation: 'vertical'},
      traces.map((trace) => this.renderTraceItem(trace, controller)),
      m(
        CardStack,
        {
          className: 'pf-multi-trace-modal__add-card',
          onclick: () => this.addTraces(controller),
        },
        m(Icon, {icon: 'add'}),
        'Add more traces',
      ),
    );
  }

  private renderTraceItem(trace: TraceFile, controller: MultiTraceController) {
    return m(
      CardStack,
      {
        className: 'pf-multi-trace-modal__card',
        direction: 'horizontal',
        key: trace.uuid,
      },
      this.renderTraceInfo(trace, controller),
      this.renderCardActions(trace, controller),
    );
  }

  private renderTraceInfo(trace: TraceFile, controller: MultiTraceController) {
    return m(
      Stack,
      {
        className: 'pf-multi-trace-modal__info',
        spacing: 'large',
        orientation: 'vertical',
      },
      m('.pf-multi-trace-modal__name', trace.file.name),
      m(
        Stack,
        {orientation: 'horizontal', spacing: 'large'},
        m(
          Stack,
          {
            className: 'pf-multi-trace-modal__size',
            orientation: 'horizontal',
          },
          m('strong', 'Size:'),
          m('span', `${(trace.file.size / (1024 * 1024)).toFixed(1)} MB`),
        ),
        trace.status === 'analyzed'
          ? m(
              Stack,
              {
                className: 'pf-multi-trace-modal__format',
                orientation: 'horizontal',
              },
              m('strong', 'Format:'),
              m('span', trace.format),
            )
          : this.renderTraceStatus(trace),
      ),
      trace.status === 'analyzed' && this.renderSyncStatus(trace, controller),
    );
  }

  private renderSyncStatus(
    trace: TraceFileAnalyzed,
    controller: MultiTraceController,
  ) {
    return m(
      '.pf-multi-trace-modal__sync-status',
      m(Icon, {icon: 'sync'}),
      this.renderSyncSummary(trace, controller),
    );
  }

  private renderSyncSummary(
    trace: TraceFileAnalyzed,
    controller: MultiTraceController,
  ): m.Children {
    const config = trace.syncConfig;

    switch (config.syncMode) {
      case 'CALCULATING':
        return m('span', 'Waiting for other traces to be analyzed...');
      case 'REFERENCE':
        return m('span', [
          'Reference clock: ',
          m('strong', config.referenceClock ?? '[Select a clock]'),
        ]);
      case 'SYNC_TO_OTHER': {
        const anchorTrace = controller.traces.find(
          (t) => t.uuid === config.syncClock?.anchorTraceUuid,
        );
        const anchorName = anchorTrace?.file.name ?? '[Select a trace]';
        const thisTraceClock =
          config.syncClock?.thisTraceClock ?? '[Select a clock]';
        const anchorClock = config.syncClock?.anchorClock ?? '[Select a clock]';
        const offset =
          config.syncClock.offset.kind === 'valid' &&
          config.syncClock.offset.value !== 0
            ? ` (offset: ${config.syncClock.offset.raw} ns)`
            : '';
        return m('span', [
          'Sync: ',
          m('strong', thisTraceClock),
          ' → ',
          m('strong', anchorClock),
          ' in ',
          m('span.pf-multi-trace-modal__sync-target', anchorName),
          offset,
        ]);
      }
      default:
        return m('span', 'Unknown sync state');
    }
  }

  private renderCardActions(
    trace: TraceFile,
    controller: MultiTraceController,
  ) {
    return m(
      '.pf-multi-trace-modal__actions',
      m(Button, {
        icon: 'edit',
        onclick: () => controller.selectTrace(trace.uuid),
      }),
      m(Button, {
        icon: 'delete',
        onclick: () => controller.removeTrace(trace.uuid),
        disabled: controller.isAnalyzing(),
      }),
    );
  }

  private renderTraceStatus(trace: TraceFile) {
    const statusInfo = getStatusInfo(trace.status);
    const progressText =
      trace.status === 'analyzing'
        ? ` (${(trace.progress * 100).toFixed(0)}%)`
        : '';
    return m(
      Stack,
      {
        orientation: 'horizontal',
        className: 'pf-multi-trace-modal__status-wrapper',
        spacing: 'small',
      },
      trace.status === 'analyzing' && m(Spinner),
      m(
        '.pf-multi-trace-modal__status' + statusInfo.class,
        `${statusInfo.text}${progressText}`,
      ),
    );
  }

  private addTraces(controller: MultiTraceController) {
    const input = document.createElement('input');
    input.type = 'file';
    input.multiple = true;
    input.addEventListener('change', () => {
      if (input.files) {
        controller.addFiles([...input.files]);
      }
    });
    input.click();
  }
}

// =============================================================================
// Trace Details Panel Component
// =============================================================================

interface TraceDetailsPanelComponentAttrs {
  trace: TraceFile;
  controller: MultiTraceController;
}

class TraceDetailsPanelComponent
  implements m.ClassComponent<TraceDetailsPanelComponentAttrs>
{
  view({attrs}: m.Vnode<TraceDetailsPanelComponentAttrs>) {
    const {trace} = attrs;
    return m(
      Stack,
      {
        className: 'pf-multi-trace-modal__details-panel',
        orientation: 'vertical',
      },
      m('h3.pf-multi-trace-modal__details-header', trace.file.name),
      this.renderDetailsPanelContent(attrs),
    );
  }

  private renderDetailsPanelContent({
    trace,
    controller,
  }: TraceDetailsPanelComponentAttrs): m.Children {
    switch (trace.status) {
      case 'analyzed':
        return this.renderAnalyzedDetails(trace, controller);
      case 'analyzing':
        return this.renderAnalyzingDetails(trace);
      case 'error':
        return this.renderErrorDetails(trace);
      case 'not-analyzed':
        return this.renderNotAnalyzedDetails();
      default:
        return m('span', 'Unknown trace status');
    }
  }

  private renderAnalyzingDetails(trace: TraceFile): m.Children {
    const progress = 'progress' in trace ? trace.progress : 0;
    return m(
      Stack,
      {
        className: 'pf-multi-trace-modal__analyzing-details',
        orientation: 'vertical',
      },
      m(Spinner),
      m('span', `Analyzing... ${(progress * 100).toFixed(0)}%`),
    );
  }

  private renderErrorDetails(trace: TraceFile): m.Children {
    const error = 'error' in trace ? trace.error : 'Unknown error';
    return m('span', `Error: ${error}`);
  }

  private renderNotAnalyzedDetails(): m.Children {
    return m('span', 'This trace has not been analyzed yet.');
  }

  private renderAnalyzedDetails(
    trace: TraceFileAnalyzed,
    controller: MultiTraceController,
  ) {
    const isManual = trace.syncMode === 'MANUAL';
    const config = trace.syncConfig;

    if (config.syncMode === 'CALCULATING') {
      return m(
        Stack,
        {
          className: 'pf-multi-trace-modal__details-content',
          orientation: 'vertical',
        },
        this.renderClockSyncMethodSelector(trace, controller),
        m('span', 'Waiting for other traces to be analyzed...'),
      );
    }

    return m(
      Stack,
      {
        className: 'pf-multi-trace-modal__details-content',
      },
      this.renderClockSyncMethodSelector(trace, controller),
      this.renderDetailRow(
        'Alignment Method',
        isManual
          ? this.renderAlignmentMethodSelector(trace, controller)
          : m(
              'span.pf-multi-trace-modal__static-select',
              config.syncMode === 'REFERENCE'
                ? 'Reference clock provider'
                : 'Anchored to another trace',
            ),
      ),
      config.syncMode === 'REFERENCE'
        ? this.renderReferenceTraceDetails(trace, controller, isManual)
        : this.renderSyncedTraceDetails(trace, controller, isManual),
    );
  }

  private renderReferenceTraceDetails(
    trace: TraceFileAnalyzed,
    controller: MultiTraceController,
    isManual: boolean,
  ) {
    if (trace.syncConfig.syncMode !== 'REFERENCE') return [];
    const config = trace.syncConfig;
    return [
      this.renderDetailRow(
        'Reference clock',
        isManual
          ? this.renderReferenceClockSelector(trace, controller)
          : m(
              'span.pf-multi-trace-modal__static-select',
              config.referenceClock,
            ),
      ),
    ];
  }

  private renderSyncedTraceDetails(
    trace: TraceFileAnalyzed,
    controller: MultiTraceController,
    isManual: boolean,
  ) {
    if (trace.syncConfig.syncMode !== 'SYNC_TO_OTHER') return [];

    if (isManual) {
      return this.renderManualSyncDetails(trace, controller, trace.syncConfig);
    } else {
      const config = trace.syncConfig;
      const anchorTraceName =
        controller.traces.find(
          (t) => t.uuid === config.syncClock.anchorTraceUuid,
        )?.file.name ?? 'Not set';
      return [
        this.renderDetailRow(
          "This trace's clock",
          m(
            'span.pf-multi-trace-modal__static-select',
            config.syncClock.thisTraceClock ?? 'Not set',
          ),
        ),
        this.renderDetailRow(
          'Anchor trace',
          m('span.pf-multi-trace-modal__static-select', anchorTraceName),
        ),
        this.renderDetailRow(
          'Anchor clock',
          m(
            'span.pf-multi-trace-modal__static-select',
            config.syncClock.anchorClock ?? 'Not set',
          ),
        ),
        this.renderDetailRow(
          'Offset (ns)',
          m(
            'span.pf-multi-trace-modal__static-select',
            config.syncClock.offset.raw,
          ),
        ),
      ];
    }
  }

  private renderManualSyncDetails(
    trace: TraceFileAnalyzed,
    controller: MultiTraceController,
    config: AnchoredConfig,
  ) {
    const anchorLink = config.syncClock;
    const otherTraces = controller.traces.filter((t) => t.uuid !== trace.uuid);

    const rawAnchorTrace = anchorLink.anchorTraceUuid
      ? controller.traces.find((t) => t.uuid === anchorLink.anchorTraceUuid)
      : undefined;

    let anchorTrace: TraceFileAnalyzed | undefined = undefined;
    if (rawAnchorTrace?.status === 'analyzed') {
      anchorTrace = rawAnchorTrace;
    }

    return [
      this.renderDetailRow(
        "This trace's clock",
        this.renderThisTraceClockSelector(trace, controller, anchorLink),
      ),
      this.renderDetailRow(
        'Anchor trace',
        this.renderAnchorTraceSelector(controller, anchorLink, otherTraces),
      ),
      this.renderDetailRow(
        'Anchor clock',
        this.renderAnchorClockSelector(controller, anchorLink, anchorTrace),
      ),
      this.renderDetailRow(
        'Offset (ns)',
        m(TextInput, {
          className: 'pf-multi-trace-modal__offset-input',
          type: 'text',
          value: anchorLink.offset.raw,
          oninput: (e: Event) => {
            const target = e.target as HTMLInputElement;
            controller.setTraceOffset(anchorLink, target.value);
            redrawModal();
          },
        }),
      ),
      anchorLink.offset.kind === 'invalid' &&
        m(
          Callout,
          {
            className: 'pf-multi-trace-modal__offset-error',
            intent: Intent.Danger,
            icon: 'error',
          },
          anchorLink.offset.error,
        ),
    ];
  }

  private renderDetailRow(label: string, content: m.Children) {
    return m(
      Stack,
      {className: 'pf-multi-trace-modal__form-group'},
      m('label', label),
      content,
    );
  }

  private renderAlignmentMethodSelector(
    trace: TraceFileAnalyzed,
    controller: MultiTraceController,
  ) {
    return m(
      Select,
      {
        className: 'pf-multi-trace-modal__select',
        value: trace.syncConfig.syncMode,
        onchange: (e: Event) => {
          const target = e.target as HTMLSelectElement;
          const newSyncMode = target.value as SyncConfig['syncMode'];
          if (newSyncMode === 'REFERENCE') {
            trace.syncConfig = {
              syncMode: 'REFERENCE',
              referenceClock: controller.findBestClock(trace),
            };
          } else {
            trace.syncConfig = {
              syncMode: 'SYNC_TO_OTHER',
              syncClock: {
                thisTraceClock: undefined,
                anchorTraceUuid: undefined,
                anchorClock: undefined,
                offset: {kind: 'valid', raw: '0', value: 0},
              },
            };
          }
          controller.recomputeSync();
          redrawModal();
        },
      },
      m('option', {value: 'REFERENCE'}, 'Use as reference'),
      m('option', {value: 'SYNC_TO_OTHER'}, 'Sync to another trace'),
    );
  }

  private renderReferenceClockSelector(
    trace: TraceFileAnalyzed,
    controller: MultiTraceController,
  ) {
    if (trace.syncConfig.syncMode !== 'REFERENCE') return;
    return m(
      Select,
      {
        className: 'pf-multi-trace-modal__select',
        value: trace.syncConfig.referenceClock ?? '',
        onchange: (e: Event) => {
          const target = e.target as HTMLSelectElement;
          trace.syncConfig = {
            syncMode: 'REFERENCE',
            referenceClock: target.value || undefined,
          };
          controller.recomputeSync();
        },
      },
      m('option', {value: ''}, 'Select a clock'),
      trace.clocks.map((clock) => m('option', {value: clock.name}, clock.name)),
    );
  }

  private renderThisTraceClockSelector(
    trace: TraceFileAnalyzed,
    controller: MultiTraceController,
    anchorLink: AnchorLink,
  ) {
    return m(
      Select,
      {
        className: 'pf-multi-trace-modal__select',
        value: anchorLink.thisTraceClock ?? '',
        onchange: (e: Event) => {
          const target = e.target as HTMLSelectElement;
          anchorLink.thisTraceClock = target.value || undefined;
          controller.recomputeSync();
        },
      },
      m('option', {value: ''}, 'Select a clock'),
      (trace.clocks ?? []).map((clock) =>
        m('option', {value: clock.name}, clock.name),
      ),
    );
  }

  private renderAnchorTraceSelector(
    controller: MultiTraceController,
    anchorLink: AnchorLink,
    otherTraces: TraceFile[],
  ) {
    const otherAnalyzedTraces = otherTraces.filter(
      (t): t is TraceFileAnalyzed => t.status === 'analyzed',
    );
    return m(
      Select,
      {
        className: 'pf-multi-trace-modal__select',
        value: anchorLink.anchorTraceUuid ?? '',
        onchange: (e: Event) => {
          const target = e.target as HTMLSelectElement;
          anchorLink.anchorTraceUuid = target.value || undefined;
          anchorLink.anchorClock = undefined; // Reset target clock when target trace changes
          controller.recomputeSync();
          redrawModal();
        },
      },
      m('option', {value: ''}, 'Select a trace'),
      otherAnalyzedTraces.map((other) =>
        m('option', {value: other.uuid}, other.file.name),
      ),
    );
  }

  private renderAnchorClockSelector(
    controller: MultiTraceController,
    anchorLink: AnchorLink,
    anchorTrace: TraceFileAnalyzed | undefined,
  ) {
    const isAnchorTraceSelected = !!anchorLink.anchorTraceUuid;

    return m(
      Select,
      {
        className: 'pf-multi-trace-modal__select',
        disabled: !isAnchorTraceSelected,
        value: anchorLink.anchorClock ?? '',
        onchange: (e: Event) => {
          const target = e.target as HTMLSelectElement;
          anchorLink.anchorClock = target.value || undefined;
          controller.recomputeSync();
        },
      },
      isAnchorTraceSelected && anchorTrace
        ? [
            m('option', {value: ''}, 'Select a clock'),
            anchorTrace.clocks.map((clock) =>
              m('option', {value: clock.name}, clock.name),
            ),
          ]
        : m(
            'option',
            {value: '', disabled: true, selected: true},
            'Select an anchor trace first',
          ),
    );
  }

  private renderClockSyncMethodSelector(
    trace: TraceFileAnalyzed,
    controller: MultiTraceController,
  ) {
    return this.renderDetailRow(
      'Clock Sync Method',
      m(Switch, {
        label: 'Automatic',
        labelLeft: 'Manual',
        checked: trace.syncMode === 'AUTOMATIC',
        onchange: (e: Event) => {
          const target = e.target as HTMLInputElement;
          trace.syncMode = target.checked ? 'AUTOMATIC' : 'MANUAL';
          controller.recomputeSync();
        },
      }),
    );
  }
}

// =============================================================================
// Public API & Helpers
// =============================================================================

export function showMultiTraceModal(initialFiles: ReadonlyArray<File>) {
  showModal({
    title: 'Open Multiple Traces',
    icon: 'library_books',
    key: MODAL_KEY,
    className: 'pf-multi-trace-modal-override',
    content: () => m(MultiTraceModalShell, {initialFiles}),
  });
}

function getStatusInfo(status: TraceStatus) {
  switch (status) {
    case 'analyzed':
      return {
        class: '.pf-multi-trace-modal__status--analyzed',
        text: 'Analyzed',
      };
    case 'analyzing':
      return {
        class: '.pf-multi-trace-modal__status--analyzing',
        text: 'Analyzing...',
      };
    case 'not-analyzed':
      return {
        class: '',
        text: 'Not analyzed',
      };
    case 'error':
      return {
        class: '.pf-multi-trace-modal__status--error',
        text: 'Error',
      };
    default:
      return {
        class: '',
        text: 'Unknown',
      };
  }
}
