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
import {copyToClipboard} from '../../base/clipboard';
import {download} from '../../base/download_utils';
import {tarFileListToBlob} from '../../core/trace_stream';
import {Anchor} from '../../widgets/anchor';
import {Button, ButtonVariant} from '../../widgets/button';
import {CardStack} from '../../widgets/card';
import {Intent} from '../../widgets/common';
import {Icon} from '../../widgets/icon';
import {closeModal, redrawModal, showModal} from '../../widgets/modal';
import {Callout} from '../../widgets/callout';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {Spinner} from '../../widgets/spinner';
import {Stack} from '../../widgets/stack';
import {TabStrip, type TabOption} from '../../widgets/tab_strip';
import {TextInput} from '../../widgets/text_input';
import {Tooltip} from '../../widgets/tooltip';
import {TextParagraph} from '../../widgets/text_paragraph';
import {MultiTraceController} from './multi_trace_controller';
import type {AlignMode, ClockName, TraceFile} from './multi_trace_types';
import type {AlignmentVerdict} from './trace_analyzer';
import {WasmTraceAnalyzer} from './trace_analyzer';

const MODAL_KEY = 'multi-trace-modal';

function renderHelp(text: string) {
  return m(
    Tooltip,
    {
      trigger: m(Icon, {
        className: 'pf-multi-trace-modal__help',
        icon: 'help_outline',
      }),
    },
    text,
  );
}

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
  private currentTab = 'merge';

  oncreate({attrs}: m.Vnode<MultiTraceModalAttrs>) {
    this.controller.addFiles(attrs.initialFiles);
  }

  view() {
    return m(
      Stack,
      {className: 'pf-multi-trace-modal', orientation: 'vertical'},
      this.renderDescription(),
      this.currentTab === 'merge' && this.renderLoadingError(),
      this.currentTab === 'merge' &&
        m(MergeConfigurator, {controller: this.controller}),
      m(
        Stack,
        {className: 'pf-multi-trace-modal__footer', orientation: 'horizontal'},
        this.renderActions(),
      ),
    );
  }

  private renderDescription() {
    const tabs: TabOption[] = [
      {key: 'merge', title: 'At the same time'},
      {key: 'comparison', title: 'Trace Comparison'},
    ];

    return m(
      Stack,
      {
        className: 'pf-multi-trace-modal__description-panel',
        orientation: 'vertical',
      },
      m(TabStrip, {
        className: 'pf-multi-trace-modal__tabs',
        tabs,
        currentTabKey: this.currentTab,
        onTabChange: (key: string) => {
          this.currentTab = key;
          redrawModal();
        },
      }),
      m('.pf-multi-trace-modal__description-content', this.renderTabContent()),
    );
  }

  private renderTabContent() {
    switch (this.currentTab) {
      case 'merge':
        return [
          m(TextParagraph, {
            text:
              'Combine traces that were captured at the same time (on one ' +
              'device or across several) onto a single shared timeline. ' +
              'Each file is placed automatically where its clocks line up; ' +
              'where they cannot, you can tell Perfetto how, per file.',
          }),
        ];
      case 'comparison':
        return [
          m(TextParagraph, {
            text: '📊 Compare traces from different time periods to identify performance regressions or improvements.',
          }),
        ];
      default:
        return '';
    }
  }

  private renderActions() {
    if (this.currentTab === 'comparison') {
      return [
        m(
          Callout,
          {
            className: 'pf-multi-trace-modal__footer-error',
            intent: Intent.Danger,
            icon: 'error_outline',
          },
          [
            'This feature is not yet supported. Please +1 ',
            m(
              Anchor,
              {
                href: 'https://github.com/google/perfetto/issues/2780',
                target: '_blank',
              },
              'this GitHub issue',
            ),
            ' to prioritize development, or select "At the same time" to ' +
              'continue.',
          ],
        ),
        m('.pf-multi-trace-modal__footer-spacer'),
        m(Button, {
          label: 'Open Traces',
          intent: Intent.Primary,
          variant: ButtonVariant.Filled,
          disabled: true,
        }),
      ];
    }

    const disabled = this.controller.getLoadingError() !== undefined;
    const checking = this.controller.isCheckingAlignment;

    return [
      m(Button, {
        label: 'Check alignment',
        icon: 'rule',
        disabled: checking || disabled,
        onclick: () => this.controller.checkAlignment(),
      }),
      renderHelp(
        'Dry-runs the merge and reports whether every trace lines up on the ' +
          'shared timeline, or how many events would be dropped.',
      ),
      m('.pf-multi-trace-modal__footer-spacer'),
      m(Button, {
        label: 'Copy manifest',
        icon: 'content_copy',
        disabled,
        onclick: () => this.copyManifest(),
      }),
      m(Button, {
        label: 'Download .tar',
        icon: 'download',
        disabled,
        onclick: () => this.downloadTar(),
      }),
      m(Button, {
        label: 'Open Traces',
        intent: Intent.Primary,
        variant: ButtonVariant.Filled,
        disabled,
        onclick: () => this.openTraces(),
      }),
    ];
  }

  private renderLoadingError() {
    const error = this.controller.getLoadingError();
    if (error === undefined) {
      return undefined;
    }
    return m(
      Callout,
      {
        className: 'pf-multi-trace-modal__panel-error',
        intent: Intent.Danger,
        icon: 'error_outline',
      },
      this.errorMessage(error),
    );
  }

  private errorMessage(error: string): string {
    switch (error) {
      case 'NO_TRACES':
        return 'Add at least one trace to open.';
      case 'DUPLICATE_NAMES':
        return 'Two traces share the same file name. Remove or rename one.';
      case 'ANALYZING':
        return 'Wait for all traces to be analyzed.';
      case 'TRACE_ERROR':
        return 'Remove traces with errors before opening.';
      default:
        return 'An unknown error occurred.';
    }
  }

  private openTraces() {
    if (this.controller.traces.length === 0) {
      return;
    }
    // MULTIPLE_FILES (rather than a one-shot STREAM) retains the file list on
    // the trace source, so the merged trace stays downloadable from the
    // timeline once loaded.
    AppImpl.instance.openTraceFromMultipleFiles(
      this.controller.getMergeFileList(),
    );
    closeModal(MODAL_KEY);
  }

  private async downloadTar() {
    const blob = await tarFileListToBlob(this.controller.getMergeFileList());
    await download({
      content: blob,
      fileName: 'merged-trace.tar',
      mimeType: 'application/x-tar',
    });
  }

  private async copyManifest() {
    await copyToClipboard(this.controller.getManifestJson());
  }
}

// =============================================================================
// Merge Configurator (per-file rows)
// =============================================================================

interface MergeConfiguratorAttrs {
  controller: MultiTraceController;
}

class MergeConfigurator implements m.ClassComponent<MergeConfiguratorAttrs> {
  view({attrs}: m.Vnode<MergeConfiguratorAttrs>) {
    const {controller} = attrs;
    return m(
      Stack,
      {className: 'pf-multi-trace-modal__list-panel', orientation: 'vertical'},
      this.renderReference(controller),
      controller.traces.map((trace) => this.renderTraceItem(trace, controller)),
      m(
        CardStack,
        {
          className: 'pf-multi-trace-modal__add-card',
          onclick: () => addTraces(controller),
        },
        m(Icon, {icon: 'add'}),
        'Add more traces',
      ),
      this.renderVerdictPanel(controller),
    );
  }

  // Verdict for "Check alignment"; the button lives in the footer.
  private renderVerdictPanel(controller: MultiTraceController) {
    const verdict = controller.alignmentVerdict;
    const checking = controller.isCheckingAlignment;
    if (!checking && verdict === undefined) {
      return undefined;
    }
    return m(
      Stack,
      {
        className: 'pf-multi-trace-modal__align-panel',
        orientation: 'vertical',
        spacing: 'small',
      },
      checking &&
        m(
          Stack,
          {orientation: 'horizontal', spacing: 'small'},
          m(Spinner),
          'Checking how these traces line up...',
        ),
      !checking && verdict !== undefined && this.renderVerdict(verdict),
    );
  }

  private renderVerdict(verdict: AlignmentVerdict) {
    if (verdict.validationError !== undefined) {
      return m(
        Callout,
        {intent: Intent.Danger, icon: 'error_outline'},
        `Manifest error: ${verdict.validationError}`,
      );
    }
    if (!verdict.ok) {
      return m(
        Callout,
        {intent: Intent.Warning, icon: 'warning'},
        `${verdict.droppedEvents.toLocaleString()} events would be dropped: ` +
          'some traces share no clock with the shared timeline and will be ' +
          'omitted. Set an explicit alignment, or check the manifest.',
      );
    }
    return m(
      Callout,
      {intent: Intent.Success, icon: 'check_circle'},
      'All traces line up on the shared timeline.',
    );
  }

  // Themed dropdown, in place of a native <select>.
  private renderDropdown(
    value: string,
    options: ReadonlyArray<{value: string; label: string}>,
    onSelect: (value: string) => void,
  ) {
    const current = options.find((o) => o.value === value);
    return m(
      PopupMenu,
      {
        trigger: m(Button, {
          label: current?.label ?? value,
          rightIcon: 'arrow_drop_down',
        }),
      },
      options.map((o) =>
        m(MenuItem, {
          label: o.label,
          rightIcon: o.value === value ? 'check' : undefined,
          onclick: () => onSelect(o.value),
        }),
      ),
    );
  }

  // The single reference everything aligns to: a clock when there are multiple
  // real clocks to choose between, otherwise the baseline trace (clockless
  // sets). Hidden when there is no meaningful choice.
  private renderReference(controller: MultiTraceController) {
    const clocks = controller.availableTraceTimeOptions();
    if (clocks.length > 0) {
      return this.renderReferenceRow(
        this.renderDropdown(
          controller.traceTime.clock ?? 'auto',
          [
            {value: 'auto', label: 'Automatic (recommended)'},
            ...clocks.map((c) => ({value: c, label: c})),
          ],
          (value) =>
            controller.setTraceTimeClock(
              value === 'auto' ? undefined : (value as ClockName),
            ),
        ),
        'The clock the merged traces share. Automatic lets Perfetto choose; ' +
          'picking one projects every trace onto that clock.',
      );
    }
    const reference = controller.referenceTraceUuid();
    if (reference !== undefined) {
      return this.renderReferenceRow(
        this.renderDropdown(
          reference,
          controller.traces.map((t) => ({value: t.uuid, label: t.file.name})),
          (uuid) => controller.setAnchor(uuid),
        ),
        'The baseline trace, kept at its own timestamps. Every other trace is ' +
          'positioned relative to it.',
      );
    }
    return undefined;
  }

  private renderReferenceRow(control: m.Children, help: string) {
    return m(
      Stack,
      {
        className: 'pf-multi-trace-modal__reference-row',
        orientation: 'horizontal',
        spacing: 'small',
      },
      m('strong', 'Align to:'),
      control,
      renderHelp(help),
    );
  }

  private renderTraceItem(trace: TraceFile, controller: MultiTraceController) {
    return m(
      CardStack,
      {
        className: 'pf-multi-trace-modal__card',
        direction: 'vertical',
        key: trace.uuid,
      },
      m(
        Stack,
        {orientation: 'horizontal', className: 'pf-multi-trace-modal__row-top'},
        this.renderTraceInfo(trace),
        this.renderCardActions(trace, controller),
      ),
      this.renderConfigControls(trace, controller),
    );
  }

  private renderTraceInfo(trace: TraceFile) {
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
              m('span', trace.analysis.format),
            )
          : this.renderTraceStatus(trace),
      ),
    );
  }

  // Per-file controls, shown only where they'd change the merge.
  private renderConfigControls(
    trace: TraceFile,
    controller: MultiTraceController,
  ) {
    if (trace.status === 'error') {
      return m(
        '.pf-multi-trace-modal__config',
        m(Callout, {intent: Intent.Danger, icon: 'error_outline'}, trace.error),
      );
    }
    if (trace.status !== 'analyzed') {
      return undefined;
    }
    const a = trace.analysis;
    const children: m.Children[] = [];

    if (a.singleClock === false) {
      children.push(
        m(
          '.pf-multi-trace-modal__note',
          'Carries its own clock snapshots, aligned automatically.',
        ),
      );
    } else if (controller.traces.length >= 2) {
      if (controller.referenceTraceUuid() === trace.uuid) {
        children.push(
          m('.pf-multi-trace-modal__note', 'Baseline. Others align to this.'),
        );
      } else {
        children.push(this.renderAlignControl(trace, controller));
      }
    }

    if (children.length === 0) {
      return undefined;
    }
    return m(
      Stack,
      {
        className: 'pf-multi-trace-modal__config',
        orientation: 'horizontal',
        spacing: 'large',
      },
      children,
    );
  }

  private renderAlignControl(
    trace: TraceFile,
    controller: MultiTraceController,
  ) {
    const config = controller.getConfig(trace.uuid);
    return m(
      Stack,
      {
        className: 'pf-multi-trace-modal__control-row',
        orientation: 'horizontal',
        spacing: 'small',
      },
      m('strong', 'Align:'),
      this.renderDropdown(
        config.alignMode,
        [
          {value: 'auto', label: 'Automatic'},
          {value: 'offset', label: 'Fixed offset'},
        ],
        (value) =>
          controller.updateConfig(trace.uuid, {alignMode: value as AlignMode}),
      ),
      config.alignMode === 'offset' &&
        m(TextInput, {
          type: 'number',
          placeholder: 'offset (ns)',
          value: config.offsetNs !== undefined ? String(config.offsetNs) : '',
          onChange: (value: string) => {
            const n = Number(value);
            const valid = value.trim().length > 0 && Number.isFinite(n);
            controller.updateConfig(trace.uuid, {
              offsetNs: valid ? n : undefined,
            });
          },
        }),
      renderHelp(
        'Where this trace sits on the shared timeline. Automatic lines it up ' +
          'using its own clocks. Fixed offset shifts it by a set number of ' +
          'nanoseconds relative to the baseline trace; a positive value moves ' +
          'it later.',
      ),
    );
  }

  private renderCardActions(
    trace: TraceFile,
    controller: MultiTraceController,
  ) {
    return m(
      '.pf-multi-trace-modal__actions',
      m(Button, {
        icon: 'delete',
        onclick: () => controller.removeTrace(trace.uuid),
        disabled: controller.isAnalyzing(),
      }),
    );
  }

  private renderTraceStatus(trace: TraceFile) {
    const statusInfo = getStatusInfo(trace);
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

function addTraces(controller: MultiTraceController) {
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

function getStatusInfo(trace: TraceFile) {
  switch (trace.status) {
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
