// Copyright (C) 2018 The Android Open Source Project
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

import * as m from 'mithril';

import {Actions} from '../common/actions';
import {MeminfoCounters, StatCounters, VmstatCounters} from '../common/protos';
import {RecordConfig} from '../common/state';

import {copyToClipboard} from './clipboard';
import {globals} from './globals';
import {createPage} from './pages';

const PROC_STATS_PRESETS = [
  {label: 'never', value: null},
  {label: '100ms', value: 100},
  {label: '250ms', value: 250},
  {label: '500ms', value: 500},
];

const COUNTER_PRESETS = [
  {label: 'never', value: null},
  {label: '10ms', value: 10},
  {label: '50ms', value: 50},
  {label: '500ms', value: 500},
];

const CONFIG_PROTO_URL =
    `https://android.googlesource.com/platform/external/perfetto/+/master/protos/perfetto/config/perfetto_config.proto`;

const FTRACE_EVENTS = [
  'binder/binder_lock',
  'binder/binder_locked',
  'binder/binder_set_priority',
  'binder/binder_transaction',
  'binder/binder_transaction_alloc_buf',
  'binder/binder_transaction_received',
  'binder/binder_unlock',
  'block/block_bio_backmerge',
  'block/block_bio_bounce',
  'block/block_bio_complete',
  'block/block_bio_frontmerge',
  'block/block_bio_queue',
  'block/block_bio_remap',
  'block/block_dirty_buffer',
  'block/block_getrq',
  'block/block_plug',
  'block/block_rq_abort',
  'block/block_rq_complete',
  'block/block_rq_insert',
  'block/block_rq_issue',
  'block/block_rq_remap',
  'block/block_rq_requeue',
  'block/block_sleeprq',
  'block/block_split',
  'block/block_touch_buffer',
  'block/block_unplug',
  'cgroup/cgroup_attach_task',
  'cgroup/cgroup_destroy_root',
  'cgroup/cgroup_mkdir',
  'cgroup/cgroup_release',
  'cgroup/cgroup_remount',
  'cgroup/cgroup_rename',
  'cgroup/cgroup_rmdir',
  'cgroup/cgroup_setup_root',
  'cgroup/cgroup_transfer_tasks',
  'clk/clk_disable',
  'clk/clk_enable',
  'clk/clk_set_rate',
  'compaction/mm_compaction_begin',
  'compaction/mm_compaction_defer_compaction',
  'compaction/mm_compaction_deferred',
  'compaction/mm_compaction_defer_reset',
  'compaction/mm_compaction_end',
  'compaction/mm_compaction_finished',
  'compaction/mm_compaction_isolate_freepages',
  'compaction/mm_compaction_isolate_migratepages',
  'compaction/mm_compaction_kcompactd_sleep',
  'compaction/mm_compaction_kcompactd_wake',
  'compaction/mm_compaction_migratepages',
  'compaction/mm_compaction_suitable',
  'compaction/mm_compaction_try_to_compact_pages',
  'compaction/mm_compaction_wakeup_kcompactd',
  'ext4/ext4_allocate_blocks',
  'ext4/ext4_allocate_inode',
  'ext4/ext4_alloc_da_blocks',
  'ext4/ext4_begin_ordered_truncate',
  'ext4/ext4_collapse_range',
  'ext4/ext4_da_release_space',
  'ext4/ext4_da_reserve_space',
  'ext4/ext4_da_update_reserve_space',
  'ext4/ext4_da_write_begin',
  'ext4/ext4_da_write_end',
  'ext4/ext4_da_write_pages',
  'ext4/ext4_da_write_pages_extent',
  'ext4/ext4_direct_IO_enter',
  'ext4/ext4_direct_IO_exit',
  'ext4/ext4_discard_blocks',
  'ext4/ext4_discard_preallocations',
  'ext4/ext4_drop_inode',
  'ext4/ext4_es_cache_extent',
  'ext4/ext4_es_find_delayed_extent_range_enter',
  'ext4/ext4_es_find_delayed_extent_range_exit',
  'ext4/ext4_es_insert_extent',
  'ext4/ext4_es_lookup_extent_enter',
  'ext4/ext4_es_lookup_extent_exit',
  'ext4/ext4_es_remove_extent',
  'ext4/ext4_es_shrink',
  'ext4/ext4_es_shrink_count',
  'ext4/ext4_es_shrink_scan_enter',
  'ext4/ext4_es_shrink_scan_exit',
  'ext4/ext4_evict_inode',
  'ext4/ext4_ext_convert_to_initialized_enter',
  'ext4/ext4_ext_convert_to_initialized_fastpath',
  'ext4/ext4_ext_handle_unwritten_extents',
  'ext4/ext4_ext_in_cache',
  'ext4/ext4_ext_load_extent',
  'ext4/ext4_ext_map_blocks_enter',
  'ext4/ext4_ext_map_blocks_exit',
  'ext4/ext4_ext_put_in_cache',
  'ext4/ext4_ext_remove_space',
  'ext4/ext4_ext_remove_space_done',
  'ext4/ext4_ext_rm_idx',
  'ext4/ext4_ext_rm_leaf',
  'ext4/ext4_ext_show_extent',
  'ext4/ext4_fallocate_enter',
  'ext4/ext4_fallocate_exit',
  'ext4/ext4_find_delalloc_range',
  'ext4/ext4_forget',
  'ext4/ext4_free_blocks',
  'ext4/ext4_free_inode',
  'ext4/ext4_get_implied_cluster_alloc_exit',
  'ext4/ext4_get_reserved_cluster_alloc',
  'ext4/ext4_ind_map_blocks_enter',
  'ext4/ext4_ind_map_blocks_exit',
  'ext4/ext4_insert_range',
  'ext4/ext4_invalidatepage',
  'ext4/ext4_journalled_invalidatepage',
  'ext4/ext4_journalled_write_end',
  'ext4/ext4_journal_start',
  'ext4/ext4_journal_start_reserved',
  'ext4/ext4_load_inode',
  'ext4/ext4_load_inode_bitmap',
  'ext4/ext4_mark_inode_dirty',
  'ext4/ext4_mballoc_alloc',
  'ext4/ext4_mballoc_discard',
  'ext4/ext4_mballoc_free',
  'ext4/ext4_mballoc_prealloc',
  'ext4/ext4_mb_bitmap_load',
  'ext4/ext4_mb_buddy_bitmap_load',
  'ext4/ext4_mb_discard_preallocations',
  'ext4/ext4_mb_new_group_pa',
  'ext4/ext4_mb_new_inode_pa',
  'ext4/ext4_mb_release_group_pa',
  'ext4/ext4_mb_release_inode_pa',
  'ext4/ext4_other_inode_update_time',
  'ext4/ext4_punch_hole',
  'ext4/ext4_read_block_bitmap_load',
  'ext4/ext4_readpage',
  'ext4/ext4_releasepage',
  'ext4/ext4_remove_blocks',
  'ext4/ext4_request_blocks',
  'ext4/ext4_request_inode',
  'ext4/ext4_sync_file_enter',
  'ext4/ext4_sync_file_exit',
  'ext4/ext4_sync_fs',
  'ext4/ext4_trim_all_free',
  'ext4/ext4_trim_extent',
  'ext4/ext4_truncate_enter',
  'ext4/ext4_truncate_exit',
  'ext4/ext4_unlink_enter',
  'ext4/ext4_unlink_exit',
  'ext4/ext4_write_begin',
  'ext4/ext4_write_end',
  'ext4/ext4_writepage',
  'ext4/ext4_writepages',
  'ext4/ext4_writepages_result',
  'ext4/ext4_zero_range',
  'f2fs/f2fs_do_submit_bio',
  'f2fs/f2fs_evict_inode',
  'f2fs/f2fs_fallocate',
  'f2fs/f2fs_get_data_block',
  'f2fs/f2fs_get_victim',
  'f2fs/f2fs_iget',
  'f2fs/f2fs_iget_exit',
  'f2fs/f2fs_new_inode',
  'f2fs/f2fs_readpage',
  'f2fs/f2fs_reserve_new_block',
  'f2fs/f2fs_set_page_dirty',
  'f2fs/f2fs_submit_write_page',
  'f2fs/f2fs_sync_file_enter',
  'f2fs/f2fs_sync_file_exit',
  'f2fs/f2fs_sync_fs',
  'f2fs/f2fs_truncate',
  'f2fs/f2fs_truncate_blocks_enter',
  'f2fs/f2fs_truncate_blocks_exit',
  'f2fs/f2fs_truncate_data_blocks_range',
  'f2fs/f2fs_truncate_inode_blocks_enter',
  'f2fs/f2fs_truncate_inode_blocks_exit',
  'f2fs/f2fs_truncate_node',
  'f2fs/f2fs_truncate_nodes_enter',
  'f2fs/f2fs_truncate_nodes_exit',
  'f2fs/f2fs_truncate_partial_nodes',
  'f2fs/f2fs_unlink_enter',
  'f2fs/f2fs_unlink_exit',
  'f2fs/f2fs_vm_page_mkwrite',
  'f2fs/f2fs_write_begin',
  'f2fs/f2fs_write_checkpoint',
  'f2fs/f2fs_write_end',
  'fence/fence_destroy',
  'fence/fence_enable_signal',
  'fence/fence_init',
  'fence/fence_signaled',
  'filemap/mm_filemap_add_to_page_cache',
  'filemap/mm_filemap_delete_from_page_cache',
  'ftrace/print',
  'i2c/i2c_read',
  'i2c/i2c_reply',
  'i2c/i2c_result',
  'i2c/i2c_write',
  'i2c/smbus_read',
  'i2c/smbus_reply',
  'i2c/smbus_result',
  'i2c/smbus_write',
  'ipi/ipi_entry',
  'ipi/ipi_exit',
  'ipi/ipi_raise',
  'irq/irq_handler_entry',
  'irq/irq_handler_exit',
  'irq/softirq_entry',
  'irq/softirq_exit',
  'irq/softirq_raise',
  'kmem/alloc_pages_iommu_end',
  'kmem/alloc_pages_iommu_fail',
  'kmem/alloc_pages_iommu_start',
  'kmem/alloc_pages_sys_end',
  'kmem/alloc_pages_sys_fail',
  'kmem/alloc_pages_sys_start',
  'kmem/dma_alloc_contiguous_retry',
  'kmem/iommu_map_range',
  'kmem/iommu_sec_ptbl_map_range_end',
  'kmem/iommu_sec_ptbl_map_range_start',
  'kmem/ion_alloc_buffer_end',
  'kmem/ion_alloc_buffer_fail',
  'kmem/ion_alloc_buffer_fallback',
  'kmem/ion_alloc_buffer_start',
  'kmem/ion_cp_alloc_retry',
  'kmem/ion_cp_secure_buffer_end',
  'kmem/ion_cp_secure_buffer_start',
  'kmem/ion_heap_grow',
  'kmem/ion_heap_shrink',
  'kmem/ion_prefetching',
  'kmem/ion_secure_cma_add_to_pool_end',
  'kmem/ion_secure_cma_add_to_pool_start',
  'kmem/ion_secure_cma_allocate_end',
  'kmem/ion_secure_cma_allocate_start',
  'kmem/ion_secure_cma_shrink_pool_end',
  'kmem/ion_secure_cma_shrink_pool_start',
  'kmem/kfree',
  'kmem/kmalloc',
  'kmem/kmalloc_node',
  'kmem/kmem_cache_alloc',
  'kmem/kmem_cache_alloc_node',
  'kmem/kmem_cache_free',
  'kmem/migrate_pages_end',
  'kmem/migrate_pages_start',
  'kmem/migrate_retry',
  'kmem/mm_page_alloc',
  'kmem/mm_page_alloc_extfrag',
  'kmem/mm_page_alloc_zone_locked',
  'kmem/mm_page_free',
  'kmem/mm_page_free_batched',
  'kmem/mm_page_pcpu_drain',
  'kmem/rss_stat',
  'lowmemorykiller/lowmemory_kill',
  'mdss/mdp_cmd_kickoff',
  'mdss/mdp_cmd_pingpong_done',
  'mdss/mdp_cmd_readptr_done',
  'mdss/mdp_cmd_release_bw',
  'mdss/mdp_cmd_wait_pingpong',
  'mdss/mdp_commit',
  'mdss/mdp_compare_bw',
  'mdss/mdp_misr_crc',
  'mdss/mdp_mixer_update',
  'mdss/mdp_perf_prefill_calc',
  'mdss/mdp_perf_set_ot',
  'mdss/mdp_perf_set_panic_luts',
  'mdss/mdp_perf_set_qos_luts',
  'mdss/mdp_perf_set_wm_levels',
  'mdss/mdp_perf_update_bus',
  'mdss/mdp_sspp_change',
  'mdss/mdp_sspp_set',
  'mdss/mdp_trace_counter',
  'mdss/mdp_video_underrun_done',
  'mdss/rotator_bw_ao_as_context',
  'mdss/tracing_mark_write',
  'mm_event/mm_event_record',
  'oom/oom_score_adj_update',
  'power/clock_disable',
  'power/clock_enable',
  'power/clock_set_rate',
  'power/cpu_frequency',
  'power/cpu_frequency_limits',
  'power/cpu_idle',
  'power/suspend_resume',
  'regulator/regulator_disable',
  'regulator/regulator_disable_complete',
  'regulator/regulator_enable',
  'regulator/regulator_enable_complete',
  'regulator/regulator_enable_delay',
  'regulator/regulator_set_voltage',
  'regulator/regulator_set_voltage_complete',
  'sched/sched_blocked_reason',
  'sched/sched_cpu_hotplug',
  'sched/sched_process_exec',
  'sched/sched_process_exit',
  'sched/sched_process_fork',
  'sched/sched_process_free',
  'sched/sched_process_hang',
  'sched/sched_process_wait',
  'sched/sched_switch',
  'sched/sched_wakeup',
  'sched/sched_wakeup_new',
  'sched/sched_waking',
  'signal/signal_deliver',
  'signal/signal_generate',
  'sync/sync_pt',
  'sync/sync_timeline',
  'sync/sync_wait',
  'task/task_newtask',
  'task/task_rename',
  'vmscan/mm_vmscan_direct_reclaim_begin',
  'vmscan/mm_vmscan_direct_reclaim_end',
  'vmscan/mm_vmscan_kswapd_sleep',
  'vmscan/mm_vmscan_kswapd_wake',
  'workqueue/workqueue_activate_work',
  'workqueue/workqueue_execute_end',
  'workqueue/workqueue_execute_start',
  'workqueue/workqueue_queue_work',
  'workqueue/workqueue_queue_work',
];
FTRACE_EVENTS.sort();

const CONFIG_PRESETS = [
  {
    label: 'Investigate CPU usage',
    config: {
      durationSeconds: 10.0,
      writeIntoFile: false,
      fileWritePeriodMs: null,
      bufferSizeMb: 32.0,

      processMetadata: true,
      scanAllProcessesOnStart: false,
      procStatusPeriodMs: null,

      ftrace: true,
      ftraceEvents: [
        'print',
        'sched_switch',
      ],
      atraceApps: [],
      atraceCategories: ['sched', 'freq', 'idle'],
      ftraceDrainPeriodMs: null,
      ftraceBufferSizeKb: null,

      sysStats: false,
      meminfoPeriodMs: null,
      meminfoCounters: [],
      vmstatPeriodMs: null,
      vmstatCounters: [],
      statPeriodMs: null,
      statCounters: [],

      power: true,
      batteryPeriodMs: 1000,
      batteryCounters: ['BATTERY_COUNTER_CHARGE', 'BATTERY_COUNTER_CURRENT'],
    },
  },
  {
    label: 'Investigate memory',
    config: {
      durationSeconds: 10.0,
      writeIntoFile: false,
      fileWritePeriodMs: null,
      bufferSizeMb: 32.0,

      processMetadata: true,
      scanAllProcessesOnStart: false,
      procStatusPeriodMs: 100,

      ftrace: true,
      ftraceEvents: [
        'print',
        'sched_switch',
        'rss_stat',
        'ion_heap_shrink',
        'ion_heap_grow',
      ],
      atraceApps: [],
      atraceCategories: ['am', 'dalvik'],
      ftraceDrainPeriodMs: null,
      ftraceBufferSizeKb: null,

      sysStats: true,
      meminfoPeriodMs: 50,
      meminfoCounters: [
        'MEMINFO_MEM_AVAILABLE',
        'MEMINFO_SWAP_CACHED',
        'MEMINFO_ACTIVE',
        'MEMINFO_INACTIVE'
      ],
      vmstatPeriodMs: null,
      vmstatCounters: [],
      statPeriodMs: null,
      statCounters: [],

      power: false,
      batteryPeriodMs: null,
      batteryCounters: [],
    },
  },
  {
    label: 'empty',
    config: {
      durationSeconds: 10.0,
      writeIntoFile: false,
      fileWritePeriodMs: null,
      bufferSizeMb: 10.0,

      processMetadata: false,
      scanAllProcessesOnStart: false,
      procStatusPeriodMs: null,

      ftrace: false,
      ftraceEvents: [],
      atraceApps: [],
      atraceCategories: [],
      ftraceDrainPeriodMs: null,
      ftraceBufferSizeKb: null,

      sysStats: false,
      meminfoPeriodMs: null,
      meminfoCounters: [],
      vmstatPeriodMs: null,
      vmstatCounters: [],
      statPeriodMs: null,
      statCounters: [],

      power: false,
      batteryPeriodMs: null,
      batteryCounters: [],
    },
  },
];

const ATRACE_CATERGORIES = [
  'gfx',         'input',    'view',       'webview',    'wm',
  'am',          'sm',       'audio',      'video',      'camera',
  'hal',         'res',      'dalvik',     'rs',         'bionic',
  'power',       'pm',       'ss',         'database',   'network',
  'adb',         'vibrator', 'aidl',       'nnapi',      'sched',
  'irq',         'i2c',      'freq',       'idle',       'disk',
  'sync',        'workq',    'memreclaim', 'regulators', 'binder_driver',
  'binder_lock', 'pagecache'
];
ATRACE_CATERGORIES.sort();


const BATTERY_COUNTERS = [
  'BATTERY_COUNTER_CHARGE',
  'BATTERY_COUNTER_CAPACITY_PERCENT',
  'BATTERY_COUNTER_CURRENT',
  'BATTERY_COUNTER_CURRENT_AVG'
];
BATTERY_COUNTERS.sort();

const DURATION_HELP = `Duration to trace for`;
const BUFFER_SIZE_HELP = `Size of the ring buffer which stores the trace`;
const PROCESS_METADATA_HELP =
    `Record process names and parent child relationships`;
const FTRACE_AND_ATRACE_HELP = `Record ftrace & atrace events`;
const POWER_HELP = `Poll battery counters from the Power Management Unit`;
const SYS_STATS_HELP = ``;

function toId(label: string): string {
  return label.toLowerCase().replace(' ', '-');
}

interface CodeSampleAttrs {
  text: string;
  hardWhitespace?: boolean;
}

class CodeSample implements m.ClassComponent<CodeSampleAttrs> {
  view({attrs}: m.CVnode<CodeSampleAttrs>) {
    return m(
        '.example-code',
        m('code',
          {
            style: {
              'white-space': attrs.hardWhitespace ? 'pre' : null,
            },
          },
          attrs.text),
        m('button',
          {
            onclick: () => copyToClipboard(attrs.text),
          },
          'Copy to clipboard'));
  }
}

interface ToggleAttrs {
  label: string;
  value: boolean;
  help: string;
  enabled: boolean;
  onchange: (v: boolean) => void;
}

class Toggle implements m.ClassComponent<ToggleAttrs> {
  view({attrs}: m.CVnode<ToggleAttrs>) {
    return m(
        'label.checkbox',
        {
          title: attrs.help,
          class: attrs.enabled ? '' : 'disabled',
        },
        attrs.label,
        m('input[type="checkbox"]', {
          onchange: m.withAttr('checked', attrs.onchange),
          disabled: !attrs.enabled,
          class: attrs.enabled ? '' : 'disabled',
          checked: attrs.value,
        }));
  }
}

interface MultiSelectAttrs {
  enabled: boolean;
  label: string;
  selected: string[];
  options: string[];
  onadd: (values: string[]) => void;
  onsubtract: (values: string[]) => void;
}

class MultiSelect implements m.ClassComponent<MultiSelectAttrs> {
  view({attrs}: m.CVnode<MultiSelectAttrs>) {
    const unselected = attrs.options.filter(o => !attrs.selected.includes(o));

    const helpers: m.Children = [];

    if (attrs.selected.length > 0) {
      helpers.push(
          m('button',
            {
              disabled: !attrs.enabled,
              onclick: () => attrs.onsubtract(attrs.selected),
            },
            'Remove all'));
    } else if (attrs.options.length > 0 && attrs.options.length < 100) {
      helpers.push(
          m('button',
            {
              disabled: !attrs.enabled,
              onclick: () => attrs.onadd(unselected),
            },
            'Add all'));
    }

    return m(
        'label.multiselect',
      {
        class: attrs.enabled ? '' : 'disabled',
        for: `multiselect-${toId(attrs.label)}`,
      },
      attrs.label,
      m('div', helpers),
      m('input', {
        id: `multiselect-${toId(attrs.label)}`,
        list: toId(attrs.label),
        disabled: !attrs.enabled,
        onchange: (e: Event) => {
        const elem = e.target as HTMLInputElement;
        attrs.onadd([elem.value]);
        elem.value = '';
        },
      }),
      m('datalist',
        {
          id: toId(attrs.label),
        },
        attrs.options.filter(option => !attrs.selected.includes(option))
        .map(value => m('option', {value}))),
      m('.multiselect-selected',
        attrs.selected.map(
          selected =>
          m('button.multiselect-selected',
            {
              disabled: !attrs.enabled,
              onclick: (_: Event) => attrs.onsubtract([selected]),
            },
            selected))));
  }
}

interface NumericPreset {
  label: string;
  value: number|null;
}

interface NumericAttrs {
  label: string;
  sublabel: string;
  enabled: boolean;
  help: string;
  placeholder?: string;
  value: number|null;
  onchange: (value: null|number) => void;
  presets: NumericPreset[];
}

function toNumber(s: string): number|null {
  const n = Number(s);
  return s === '' || isNaN(n) ? null : n;
}

class Numeric implements m.ClassComponent<NumericAttrs> {
  view({attrs}: m.CVnode<NumericAttrs>) {
    return m(
        'label.range',
        {
          'for': `range-${toId(attrs.label)}`,
          'title': attrs.help,
          class: attrs.enabled ? '' : 'disabled',
        },
        attrs.label,
        m('.range-control',
          attrs.presets.map(
              p =>
                  m('button',
                    {
                      disabled: !attrs.enabled,
                      class: attrs.value === p.value ? 'selected' : '',
                      onclick: () => attrs.onchange(p.value),
                    },
                    p.label)),
          m('input[type=number][min=1]', {
            id: `range-${toId(attrs.label)}`,
            placeholder: attrs.placeholder,
            value: attrs.value,
            disabled: !attrs.enabled,
            onchange: m.withAttr('value', s => attrs.onchange(toNumber(s))),
          })),
        m('small', attrs.sublabel), );
  }
}

function onAdd(name: string) {
  return (optionsToAdd: string[]) => {
    globals.dispatch(Actions.addConfigControl({name, optionsToAdd}));
  };
}

function onSubtract(name: string) {
  return (optionsToRemove: string[]) => {
    globals.dispatch(Actions.removeConfigControl({name, optionsToRemove}));
  };
}

function onChange<T extends string|number|boolean|null>(name: string) {
  return (value: T) => {
    globals.dispatch(Actions.setConfigControl({name, value}));
  };
}

function isFalsy(x: undefined|null|number|'') {
  return x === undefined || x === null || x === 0 || x === '';
}

function isTruthy(x: undefined|null|number|'') {
  return !isFalsy(x);
}

function sameConfig(a: RecordConfig, b: RecordConfig) {
  return JSON.stringify(a) === JSON.stringify(b);
}

export const RecordPage = createPage({
  view() {
    const state = globals.state;
    const config = globals.state.recordConfig;
    const data = globals.trackDataStore.get('config') as {
      commandline: string,
      pbtxt: string,
    } | null;
    return m(
      '.record-page',
        {class: state.displayConfigAsPbtxt ? 'three' : 'two' },
        m('.config.text-column',
          `To collect a Perfetto trace, use one of the following preset configs
          or customize the config manually before using the command on the
          right to capture the trace.`,

          m('.heading.config-presets',
            m('i', 'Presets'),
            CONFIG_PRESETS.map(preset => m('button', {
              onclick: () => globals.dispatch(
                Actions.setConfig({config: preset.config})),
              class: sameConfig(preset.config, config) ? 'selected' : '',
            }, preset.label))),

          m('.heading',
            m('i', 'Configuration'),
            m(Numeric, {
            enabled: true,
            label: 'Duration',
            sublabel: 's',
            placeholder: '',
            value: config.durationSeconds,
            help: DURATION_HELP,
            onchange: onChange<number|null>('durationSeconds'),
            presets: [
              {label: '10s', value: 10},
              {label: '1m', value: 60},
            ]
          })),

          m(Toggle, {
            label: 'Long trace mode',
            help: '',
            value: config.writeIntoFile,
            enabled: true,
            onchange: onChange<boolean>('writeIntoFile'),
          }),
          m('.control-group', m(Numeric, {
              enabled: config.writeIntoFile,
              label: 'Flush into file every',
              sublabel: 'ms',
              placeholder: 'default',
              value: config.fileWritePeriodMs,
              help: '',
              onchange: onChange<number|null>('fileWritePeriodMs'),
              presets: [
                {label: '5000ms', value: 5000},
              ]
            }), ),

          m(Numeric, {
            enabled: true,
            label: 'Buffer size',
            sublabel: 'mb',
            help: BUFFER_SIZE_HELP,
            placeholder: '',
            value: config.bufferSizeMb,
            onchange: onChange<number|null>('bufferSizeMb'),
            presets: [
              {label: '8mb', value: 8},
              {label: '32mb', value: 32},
              {label: '128mb', value: 128},
            ]
          }),

          // TODO(hjd): Re-add when multi-buffer support comes.
          //m('.control-group', m(Toggle, {
          //    label: 'Scan all processes on start',
          //    value: config.scanAllProcessesOnStart,
          //    help: SCAN_ALL_PROCESSES_ON_START_HELP,
          //    enabled: config.processMetadata,
          //    onchange: onChange<boolean>('scanAllProcessesOnStart'),
          //})),

          m('.heading', m(Toggle, {
            label: 'Ftrace & Atrace',
            value: config.ftrace,
            enabled: true,
            help: FTRACE_AND_ATRACE_HELP,
            onchange: onChange<boolean>('ftrace'),
          })),

          m('.control-group',
            m(MultiSelect, {
              label: 'Ftrace Events',
              enabled: config.ftrace,
              selected: config.ftraceEvents,
              options: FTRACE_EVENTS,
              onadd: onAdd('ftraceEvents'),
              onsubtract: onSubtract('ftraceEvents'),
            }),

            m(MultiSelect, {
              label: 'Atrace Categories',
              enabled: config.ftrace,
              selected: config.atraceCategories,
              options: ATRACE_CATERGORIES,
              onadd: onAdd('atraceCategories'),
              onsubtract: onSubtract('atraceCategories'),
            }),

            m(MultiSelect, {
              label: 'Atrace Apps',
              enabled: config.ftrace,
              selected: config.atraceApps,
              options: [],
              onadd: onAdd('atraceApps'),
              onsubtract: onSubtract('atraceApps'),
            }),

            m('i', {
              class: config.ftrace ? '' : 'disabled'
            }, 'Advanced ftrace configuration'),

            m(Numeric, {
              enabled: config.ftrace,
              label: 'Drain kernel buffer every',
              sublabel: 'ms',
              help: '',
              placeholder: 'default',
              value: config.ftraceDrainPeriodMs,
              onchange: onChange<number|null>('ftraceDrainPeriodMs'),
              presets: [
                {label: '100ms', value: 100},
                {label: '500ms', value: 500},
                {label: '1000ms', value: 1000},
              ]
            }),

            m(Numeric, {
              enabled: config.ftrace,
              label: 'Kernel buffer size (per cpu)',
              sublabel: 'kb',
              help: '',
              placeholder: 'default',
              value: config.ftraceBufferSizeKb,
              onchange: onChange<number|null>('ftraceBufferSizeKb'),
              presets: [
                {label: '1mb', value: 1 * 1024},
                {label: '4mb', value: 4 * 1024},
                {label: '8mb', value: 8 * 1024},
              ]
            }),

            ),

          m('.heading', m(Toggle, {
            label: 'Per-process stats and thread associations',
            help: PROCESS_METADATA_HELP,
            value: config.processMetadata,
            enabled: true,
            onchange: onChange<boolean|null>('processMetadata'),
          })),

          m('.control-group',
            m(Numeric, {
              label: 'Poll /proc/[pid]/status',
              sublabel: 'ms',
              enabled: config.processMetadata,
              help: '',
              placeholder: 'never',
              value: config.procStatusPeriodMs,
              onchange: onChange<null|number>('procStatusPeriodMs'),
              presets: PROC_STATS_PRESETS,
            }),
          ),

          m('.heading', m(Toggle, {
            label: 'System-wide stats (/proc poller)',
            value: config.sysStats,
            enabled: true,
            help: SYS_STATS_HELP,
            onchange: onChange<boolean>('sysStats'),
          })),

          m('.control-group',

            m(Numeric, {
              label: 'Poll /proc/meminfo',
              sublabel: 'ms',
              enabled: config.sysStats,
              help: '',
              placeholder: 'never',
              value: config.meminfoPeriodMs,
              onchange: onChange<null|number>('meminfoPeriodMs'),
              presets: COUNTER_PRESETS,
            }),

            m(MultiSelect, {
              label: 'Meminfo Counters',
              enabled: config.sysStats && isTruthy(config.meminfoPeriodMs),
              selected: config.meminfoCounters,
              options: Object.keys(MeminfoCounters)
                           .filter(c => c !== 'MEMINFO_UNSPECIFIED'),
              onadd: onAdd('meminfoCounters'),
              onsubtract: onSubtract('meminfoCounters'),
            }),

            m(Numeric, {
              label: 'Poll /proc/vmstat',
              sublabel: 'ms',
              enabled: config.sysStats,
              help: '',
              placeholder: 'never',
              value: config.vmstatPeriodMs,
              onchange: onChange<null|number>('vmstatPeriodMs'),
              presets: COUNTER_PRESETS,
            }),

            m(MultiSelect, {
              label: 'Vmstat Counters',
              enabled: config.sysStats && isTruthy(config.vmstatPeriodMs),
              selected: config.vmstatCounters,
              options: Object.keys(VmstatCounters)
                           .filter(c => c !== 'VMSTAT_UNSPECIFIED'),
              onadd: onAdd('vmstatCounters'),
              onsubtract: onSubtract('vmstatCounters'),
            }),

            m(Numeric, {
              label: 'Poll /proc/stat',
              sublabel: 'ms',
              enabled: config.sysStats,
              help: '',
              placeholder: 'never',
              value: config.statPeriodMs,
              onchange: onChange<null|number>('statPeriodMs'),
              presets: COUNTER_PRESETS,
            }),

            m(MultiSelect, {
              label: 'Stat Counters',
              enabled: config.sysStats && isTruthy(config.statPeriodMs),
              selected: config.statCounters,
              options: Object.keys(StatCounters)
                           .filter(c => c !== 'STAT_UNSPECIFIED'),
              onadd: onAdd('statCounters'),
              onsubtract: onSubtract('statCounters'),
            }),

            ),

            m('.heading', m(Toggle, {
              label: 'Battery and power',
              help: POWER_HELP,
              value: config.power,
              enabled: true,
              onchange: onChange<boolean|null>('power'),
            })),

            m('.control-group',
              m(Numeric, {
                enabled: config.power,
                label: 'Polling rate',
                sublabel: 'ms',
                help: '',
                placeholder: 'never',
                value: config.batteryPeriodMs,
                onchange: onChange<number|null>('batteryPeriodMs'),
                presets: [
                  {label: '1000ms', value: 1000},
                  {label: '5000ms', value: 5000},
                  {label: '10000ms', value: 10000},
                ]
              }),
              m(MultiSelect, {
                label: 'Battery counters',
                enabled: config.power && isTruthy(config.batteryPeriodMs),
                selected: config.batteryCounters,
                options: BATTERY_COUNTERS,
                onadd: onAdd('batteryCounters'),
                onsubtract: onSubtract('batteryCounters'),
              }),
            ),

          m('hr'),

          m(Toggle, {
            label: 'Display config as pbtxt',
            value: state.displayConfigAsPbtxt,
            enabled: true,
            help: '',
            onchange: () => {
              globals.dispatch(Actions.toggleDisplayConfigAsPbtxt({}));
            },
          }),


        ),

        data ?
            [
              m('.command.text-column',
                `To collect a ${config.durationSeconds}
                second Perfetto trace from an Android phone run this command:`,
                m(CodeSample, {text: data.commandline}),
                'Then click "Open trace file" in the menu to the left and select',
                ' "/tmp/trace".'),

              state.displayConfigAsPbtxt ?
                m('.pbtxt.text-column',
                  `A Perfetto config controls what and how much information is
                  collected. It is encoded as a `,
                  m('a', {href: CONFIG_PROTO_URL}, 'proto'), '.',
                  m(CodeSample, {text: data.pbtxt, hardWhitespace: true})
                ) : null,
            ] : null,

);


  }
});
