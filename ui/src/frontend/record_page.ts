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

import {copyToClipboard} from './clipboard';
import {globals} from './globals';
import {createPage} from './pages';

const CONFIG_PROTO_URL =
    `https://android.googlesource.com/platform/external/perfetto/+/master/protos/perfetto/config/perfetto_config.proto`;

const FTRACE_EVENTS = [
  'print',
  'sched_switch',
  'cpufreq_interactive_already',
  'cpufreq_interactive_boost',
  'cpufreq_interactive_notyet',
  'cpufreq_interactive_setspeed',
  'cpufreq_interactive_target',
  'cpufreq_interactive_unboost',
  'cpu_frequency',
  'cpu_frequency_limits',
  'cpu_idle',
  'clock_enable',
  'clock_disable',
  'clock_set_rate',
  'sched_wakeup',
  'sched_blocked_reason',
  'sched_cpu_hotplug',
  'sched_waking',
  'ipi_entry',
  'ipi_exit',
  'ipi_raise',
  'softirq_entry',
  'softirq_exit',
  'softirq_raise',
  'i2c_read',
  'i2c_write',
  'i2c_result',
  'i2c_reply',
  'smbus_read',
  'smbus_write',
  'smbus_result',
  'smbus_reply',
  'lowmemory_kill',
  'irq_handler_entry',
  'irq_handler_exit',
  'sync_pt',
  'sync_timeline',
  'sync_wait',
  'ext4_da_write_begin',
  'ext4_da_write_end',
  'ext4_sync_file_enter',
  'ext4_sync_file_exit',
  'block_rq_issue',
  'mm_vmscan_direct_reclaim_begin',
  'mm_vmscan_direct_reclaim_end',
  'mm_vmscan_kswapd_wake',
  'mm_vmscan_kswapd_sleep',
  'binder_transaction',
  'binder_transaction_received',
  'binder_set_priority',
  'binder_lock',
  'binder_locked',
  'binder_unlock',
  'workqueue_activate_work',
  'workqueue_execute_end',
  'workqueue_execute_start',
  'workqueue_queue_work',
  'regulator_disable',
  'regulator_disable_complete',
  'regulator_enable',
  'regulator_enable_complete',
  'regulator_enable_delay',
  'regulator_set_voltage',
  'regulator_set_voltage_complete',
  'cgroup_attach_task',
  'cgroup_mkdir',
  'cgroup_remount',
  'cgroup_rmdir',
  'cgroup_transfer_tasks',
  'cgroup_destroy_root',
  'cgroup_release',
  'cgroup_rename',
  'cgroup_setup_root',
  'mdp_cmd_kickoff',
  'mdp_commit',
  'mdp_perf_set_ot',
  'mdp_sspp_change',
  'tracing_mark_write',
  'mdp_cmd_pingpong_done',
  'mdp_compare_bw',
  'mdp_perf_set_panic_luts',
  'mdp_sspp_set',
  'mdp_cmd_readptr_done',
  'mdp_misr_crc',
  'mdp_perf_set_qos_luts',
  'mdp_trace_counter',
  'mdp_cmd_release_bw',
  'mdp_mixer_update',
  'mdp_perf_set_wm_levels',
  'mdp_video_underrun_done',
  'mdp_cmd_wait_pingpong',
  'mdp_perf_prefill_calc',
  'mdp_perf_update_bus',
  'rotator_bw_ao_as_context',
  'mm_filemap_add_to_page_cache',
  'mm_filemap_delete_from_page_cache',
  'mm_compaction_begin',
  'mm_compaction_defer_compaction',
  'mm_compaction_deferred',
  'mm_compaction_defer_reset',
  'mm_compaction_end',
  'mm_compaction_finished',
  'mm_compaction_isolate_freepages',
  'mm_compaction_isolate_migratepages',
  'mm_compaction_kcompactd_sleep',
  'mm_compaction_kcompactd_wake',
  'mm_compaction_migratepages',
  'mm_compaction_suitable',
  'mm_compaction_try_to_compact_pages',
  'mm_compaction_wakeup_kcompactd',
  'suspend_resume',
  'sched_wakeup_new',
  'block_bio_backmerge',
  'block_bio_bounce',
  'block_bio_complete',
  'block_bio_frontmerge',
  'block_bio_queue',
  'block_bio_remap',
  'block_dirty_buffer',
  'block_getrq',
  'block_plug',
  'block_rq_abort',
  'block_rq_complete',
  'block_rq_insert',
  '  removed',
  'block_rq_remap',
  'block_rq_requeue',
  'block_sleeprq',
  'block_split',
  'block_touch_buffer',
  'block_unplug',
  'ext4_alloc_da_blocks',
  'ext4_allocate_blocks',
  'ext4_allocate_inode',
  'ext4_begin_ordered_truncate',
  'ext4_collapse_range',
  'ext4_da_release_space',
  'ext4_da_reserve_space',
  'ext4_da_update_reserve_space',
  'ext4_da_write_pages',
  'ext4_da_write_pages_extent',
  'ext4_direct_IO_enter',
  'ext4_direct_IO_exit',
  'ext4_discard_blocks',
  'ext4_discard_preallocations',
  'ext4_drop_inode',
  'ext4_es_cache_extent',
  'ext4_es_find_delayed_extent_range_enter',
  'ext4_es_find_delayed_extent_range_exit',
  'ext4_es_insert_extent',
  'ext4_es_lookup_extent_enter',
  'ext4_es_lookup_extent_exit',
  'ext4_es_remove_extent',
  'ext4_es_shrink',
  'ext4_es_shrink_count',
  'ext4_es_shrink_scan_enter',
  'ext4_es_shrink_scan_exit',
  'ext4_evict_inode',
  'ext4_ext_convert_to_initialized_enter',
  'ext4_ext_convert_to_initialized_fastpath',
  'ext4_ext_handle_unwritten_extents',
  'ext4_ext_in_cache',
  'ext4_ext_load_extent',
  'ext4_ext_map_blocks_enter',
  'ext4_ext_map_blocks_exit',
  'ext4_ext_put_in_cache',
  'ext4_ext_remove_space',
  'ext4_ext_remove_space_done',
  'ext4_ext_rm_idx',
  'ext4_ext_rm_leaf',
  'ext4_ext_show_extent',
  'ext4_fallocate_enter',
  'ext4_fallocate_exit',
  'ext4_find_delalloc_range',
  'ext4_forget',
  'ext4_free_blocks',
  'ext4_free_inode',
  'ext4_get_implied_cluster_alloc_exit',
  'ext4_get_reserved_cluster_alloc',
  'ext4_ind_map_blocks_enter',
  'ext4_ind_map_blocks_exit',
  'ext4_insert_range',
  'ext4_invalidatepage',
  'ext4_journal_start',
  'ext4_journal_start_reserved',
  'ext4_journalled_invalidatepage',
  'ext4_journalled_write_end',
  'ext4_load_inode',
  'ext4_load_inode_bitmap',
  'ext4_mark_inode_dirty',
  'ext4_mb_bitmap_load',
  'ext4_mb_buddy_bitmap_load',
  'ext4_mb_discard_preallocations',
  'ext4_mb_new_group_pa',
  'ext4_mb_new_inode_pa',
  'ext4_mb_release_group_pa',
  'ext4_mb_release_inode_pa',
  'ext4_mballoc_alloc',
  'ext4_mballoc_discard',
  'ext4_mballoc_free',
  'ext4_mballoc_prealloc',
  'ext4_other_inode_update_time',
  'ext4_punch_hole',
  'ext4_read_block_bitmap_load',
  'ext4_readpage',
  'ext4_releasepage',
  'ext4_remove_blocks',
  'ext4_request_blocks',
  'ext4_request_inode',
  'ext4_sync_fs',
  'ext4_trim_all_free',
  'ext4_trim_extent',
  'ext4_truncate_enter',
  'ext4_truncate_exit',
  'ext4_unlink_enter',
  'ext4_unlink_exit',
  'ext4_write_begin',
  'ext4_write_end',
  'ext4_writepage',
  'ext4_writepages',
  'ext4_writepages_result',
  'ext4_zero_range',
  'task_newtask',
  'task_rename',
  'sched_process_exec',
  'sched_process_exit',
  'sched_process_fork',
  'sched_process_free',
  'sched_process_hang',
  'sched_process_wait',
  'f2fs_do_submit_bio',
  'f2fs_evict_inode',
  'f2fs_fallocate',
  'f2fs_get_data_block',
  'f2fs_get_victim',
  'f2fs_iget',
  'f2fs_iget_exit',
  'f2fs_new_inode',
  'f2fs_readpage',
  'f2fs_reserve_new_block',
  'f2fs_set_page_dirty',
  'f2fs_submit_write_page',
  'f2fs_sync_file_enter',
  'f2fs_sync_file_exit',
  'f2fs_sync_fs',
  'f2fs_truncate',
  'f2fs_truncate_blocks_enter',
  'f2fs_truncate_blocks_exit',
  'f2fs_truncate_data_blocks_range',
  'f2fs_truncate_inode_blocks_enter',
  'f2fs_truncate_inode_blocks_exit',
  'f2fs_truncate_node',
  'f2fs_truncate_nodes_enter',
  'f2fs_truncate_nodes_exit',
  'f2fs_truncate_partial_nodes',
  'f2fs_unlink_enter',
  'f2fs_unlink_exit',
  'f2fs_vm_page_mkwrite',
  'f2fs_write_begin',
  'f2fs_write_checkpoint',
  'f2fs_write_end',
];

const ATRACE_CATERGORIES = [
  'gfx',         'input',     'view',       'webview',    'wm',
  'am',          'sm',        'audio',      'video',      'camera',
  'hal',         'res',       'dalvik',     'rs',         'bionic',
  'power',       'pm',        'ss',         'database',   'network',
  'adb',         'vibrator',  'aidl',       'nnapi',      'sched',
  'irq',         'irqoff',    'preemptoff', 'i2c',        'freq',
  'membus',      'idle',      'disk',       'mmc',        'load',
  'sync',        'workq',     'memreclaim', 'regulators', 'binder_driver',
  'binder_lock', 'pagecache',
];

const ATRACE_APPS = [
  'com.android.chrome',
  'com.android.bluetooth',
  'com.android.chrome',
  'com.android.nfc',
  'com.android.phone',
  'com.android.settings',
  'com.android.systemui',
  'com.android.vending',
  'com.google.android.apps.messaging',
  'com.google.android.apps.nexuslauncher',
  'com.google.android.connectivitymonitor',
  'com.google.android.contacts',
  'com.google.android.gms',
  'com.google.android.gms.learning',
  'com.google.android.gms.persistent',
  'com.google.android.gms.unstable',
  'com.google.android.googlequicksearchbox',
  'com.google.android.setupwizard',
  'com.google.android.volta',
];

const DURATION_HELP = `Duration to trace for.`;
const BUFFER_SIZE_HELP = `Size of the ring buffer which stores the trace.`;
const PROCESS_METADATA_HELP =
    `Record process names and parent child relationships.`;
const SCAN_ALL_PROCESSES_ON_START_HELP =
    `When tracing begins read metadata for all processes.`;

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
          checked: attrs.value,
        }));
  }
}

interface MultiSelectAttrs {
  enabled: boolean;
  label: string;
  selected: string[];
  options: string[];
  onadd: (value: string) => void;
  onsubtract: (value: string) => void;
}

class MultiSelect implements m.ClassComponent<MultiSelectAttrs> {
  view({attrs}: m.CVnode<MultiSelectAttrs>) {
    return m(
        'label.multiselect',
        {class: attrs.enabled ? '' : 'disabled'},
        attrs.label,
        m('input', {
          list: toId(attrs.label),
          disabled: !attrs.enabled,
          onchange: (e: Event) => {
            const elem = e.target as HTMLInputElement;
            attrs.onadd(elem.value);
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
                      onclick: (_: Event) => attrs.onsubtract(selected),
                    },
                    selected))), );
  }
}

interface Preset {
  label: string;
  value: number;
}

interface NumericAttrs {
  label: string;
  sublabel: string;
  help: string;
  value: number;
  onchange: (value: number) => void;
  presets: Preset[];
}

class Numeric implements m.ClassComponent<NumericAttrs> {
  view({attrs}: m.CVnode<NumericAttrs>) {
    return m(
        'label.range',
        {
          'for': `range-${attrs.label}`,
          'title': attrs.help,
        },
        attrs.label,
        m('.range-control',
          attrs.presets.map(
              p =>
                  m('button',
                    {
                      class: attrs.value === p.value ? 'selected' : '',
                      onclick: () => attrs.onchange(p.value),
                    },
                    p.label)),
          m('input[type=number][min=0]', {
            id: `range-${attrs.label}`,
            value: attrs.value,
            onchange: m.withAttr('value', attrs.onchange),
          })),
        m('small', attrs.sublabel), );
  }
}

export const RecordPage = createPage({
  view() {
    const state = globals.state.recordConfig;
    const data = globals.trackDataStore.get('config') as {
      commandline: string,
      pbtxt: string,
    } | null;
    return m(
        '.record-page',

        m('.text-column', ),
        m('.text-column', `To collect a ${state.durationSeconds}
          second Perfetto trace from an Android phone run this command:`),
        m('.text-column',
          `A Perfetto config controls what and how much information is
        collected. It is encoded as a `,
          m('a',
            {
              href: CONFIG_PROTO_URL,
            },
            'proto'),
          '.'),

        m('.text-column',
          m(Numeric, {
            label: 'Duration',
            sublabel: 's',
            value: state.durationSeconds,
            help: DURATION_HELP,
            onchange: (value: number) => {
              globals.dispatch(
                  Actions.setConfigControl({name: 'durationSeconds', value}));
            },
            presets: [
              {label: '10s', value: 10},
              {label: '1m', value: 60},
            ]
          }),

          m(Numeric, {
            label: 'Buffer size',
            sublabel: 'mb',
            help: BUFFER_SIZE_HELP,
            value: state.bufferSizeMb,
            onchange: (value: number) => {
              globals.dispatch(
                  Actions.setConfigControl({name: 'bufferSizeMb', value}));
            },
            presets: [
              {label: '1mb', value: 1},
              {label: '10mb', value: 10},
              {label: '20mb', value: 20},
            ]
          }),

          m(Toggle, {
            label: 'Process Metadata',
            help: PROCESS_METADATA_HELP,
            value: state.processMetadata,
            enabled: true,
            onchange: (value: boolean) => {
              globals.dispatch(
                  Actions.setConfigControl({name: 'processMetadata', value}));
            },
          }),
          m('.control-group', m(Toggle, {
              label: 'Scan all processes on start',
              value: state.scanAllProcessesOnStart,
              help: SCAN_ALL_PROCESSES_ON_START_HELP,
              enabled: state.processMetadata,
              onchange: (value: boolean) => {
                globals.dispatch(Actions.setConfigControl(
                    {name: 'scanAllProcessesOnStart', value}));
              },
            }), ),

          m(Toggle, {
            label: 'Ftrace & Atrace',
            value: state.ftrace,
            enabled: true,
            help: SCAN_ALL_PROCESSES_ON_START_HELP,
            onchange: (value: boolean) => {
              globals.dispatch(
                  Actions.setConfigControl({name: 'ftrace', value}));
            },
          }),

          m('.control-group',
            m(MultiSelect, {
              label: 'Ftrace Events',
              enabled: state.ftrace,
              selected: state.ftraceEvents,
              options: FTRACE_EVENTS,
              onadd: (option: string) => {
                globals.dispatch(
                    Actions.addConfigControl({name: 'ftraceEvents', option}));
              },
              onsubtract: (option: string) => {
                globals.dispatch(Actions.removeConfigControl(
                    {name: 'ftraceEvents', option}));
              },
            }),

            m(MultiSelect, {
              label: 'Atrace Categories',
              enabled: state.ftrace,
              selected: state.atraceCategories,
              options: ATRACE_CATERGORIES,
              onadd: (option: string) => {
                globals.dispatch(Actions.addConfigControl(
                    {name: 'atraceCategories', option}));
              },
              onsubtract: (option: string) => {
                globals.dispatch(Actions.removeConfigControl(
                    {name: 'atraceCategories', option}));
              },
            }),

            m(MultiSelect, {
              label: 'Atrace Apps',
              enabled: state.ftrace,
              selected: state.atraceApps,
              options: ATRACE_APPS,
              onadd: (option: string) => {
                globals.dispatch(
                    Actions.addConfigControl({name: 'atraceApps', option}));
              },
              onsubtract: (option: string) => {
                globals.dispatch(
                    Actions.removeConfigControl({name: 'atraceApps', option}));
              },
            }), ),

          ),

        data ?
            [
              m('.text-column',
                m(CodeSample, {text: data.commandline}),
                'Then click "Open trace file" in the menu to the left and select',
                ' "/tmp/trace".', ),
              m('.text-column',
                m(CodeSample, {text: data.pbtxt, hardWhitespace: true}), ),
            ] :
            null);
  }
});
