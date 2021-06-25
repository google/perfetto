/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/kallsyms/lazy_kernel_symbolizer.h"

#include <cinttypes>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

const char kUnrestrictedKallsyms[] = R"(
0000000000000000 A fixed_percpu_data
0000000000000000 A __per_cpu_start
0000000000001000 A cpu_debug_store
0000000000002000 A irq_stack_backing_store
0000000000006000 A cpu_tss_rw
0000000000009000 A gdt_page
000000000000a000 A entry_stack_storage
000000000000b000 A exception_stacks
0000000000010000 A espfix_stack
0000000000010008 A espfix_waddr
0000000000010010 A cpu_llc_id
0000000000010020 A mce_banks_array
0000000000010220 A mce_num_banks
0000000000010228 A cpu_sibling_map
0000000000010230 A cpu_core_map
0000000000010238 A cpu_die_map
0000000000010240 A cpu_info
0000000000010330 A cpu_llc_shared_map
0000000000010338 A cpu_number
0000000000010340 A this_cpu_off
0000000000010348 A x86_cpu_to_apicid
000000000001034a A x86_bios_cpu_apicid
000000000001034c A x86_cpu_to_acpiid
0000000000010350 A sched_core_priority
0000000000011000 A svm_data
0000000000011008 A current_tsc_ratio
0000000000011010 A saved_epb
0000000000011018 A cluster_masks
0000000000011020 A x86_cpu_to_logical_apicid
0000000000011028 A ipi_mask
0000000000011030 A menu_devices
0000000000011098 A cpu_loops_per_jiffy
00000000000110a0 A cpu_hw_events
00000000000123b0 A pmc_prev_left
00000000000125b0 A perf_nmi_tstamp
0000000000013000 A bts_ctx
0000000000016000 A insn_buffer
0000000000016008 A pt_ctx
00000000000160b0 A cpu_tsc_khz
00000000000160b8 A current_vcpu
00000000000160c0 A loaded_vmcss_on_cpu
00000000000160d0 A current_vmcs
00000000000160d8 A vmxarea
00000000000160e0 A blocked_vcpu_on_cpu
00000000000160f0 A blocked_vcpu_on_cpu_lock
00000000000160f8 A irq_regs
0000000000016100 A nmi_state
0000000000016108 A nmi_cr2
0000000000016110 A update_debug_stack
0000000000016118 A last_nmi_rip
0000000000016120 A nmi_stats
0000000000016130 A swallow_nmi
0000000000016140 A vector_irq
0000000000028458 A processor_device_array
0000000000028460 A acpi_cpuidle_device
0000000000028470 A acpi_cstate
00000000000284c0 A cpufreq_thermal_reduction_pctg
00000000000284c8 A cpc_desc_ptr
00000000000284d0 A cpu_pcc_subspace_idx
00000000000284d8 A irq_randomness
00000000000284f8 A batched_entropy_u64
0000000000028540 A batched_entropy_u32
00000000000285c0 A drm_unplug_srcu_srcu_data
0000000000028740 A device_links_srcu_srcu_data
00000000000288c0 A cpu_sys_devices
00000000000288c8 A ci_cpu_cacheinfo
00000000000288e0 A ci_cache_dev
00000000000288e8 A ci_index_dev
0000000000028900 A wakeup_srcu_srcu_data
0000000000028a80 A flush_idx
0000000000028ac0 A dax_srcu_srcu_data
0000000000028c40 A cpufreq_cpu_data
0000000000028c80 A cpufreq_transition_notifier_list_head_srcu_data
0000000000028e00 A cpu_dbs
0000000000028e30 A cpuidle_devices
0000000000028e38 A cpuidle_dev
00000000000290f8 A netdev_alloc_cache
0000000000029110 A napi_alloc_cache
0000000000029330 A flush_works
0000000000029360 A bpf_redirect_info
0000000000029388 A bpf_sp
0000000000029588 A nf_skb_duplicated
000000000002958c A xt_recseq
0000000000029590 A rt_cache_stat
00000000000295b0 A tsq_tasklet
00000000000295e8 A xfrm_trans_tasklet
0000000000029628 A radix_tree_preloads
0000000000029640 A irq_stat
00000000000296c0 A cyc2ns
0000000000029700 A cpu_tlbstate
0000000000029780 A flush_tlb_info
00000000000297c0 A cpu_worker_pools
0000000000029ec0 A runqueues
000000000002aa80 A sched_clock_data
000000000002aac0 A osq_node
000000000002ab00 A qnodes
000000000002ab40 A rcu_data
000000000002ae80 A cfd_data
000000000002aec0 A call_single_queue
000000000002af00 A csd_data
000000000002af40 A softnet_data
000000000002b200 A rt_uncached_list
000000000002b240 A rt6_uncached_list
000000000002b258 A __per_cpu_end
ffffffffb7e00000 T startup_64
ffffffffb7e00000 T _stext
ffffffffb7e00000 T _text
ffffffffb7e00030 T secondary_startup_64
ffffffffb7e000e0 T verify_cpu
ffffffffb7e001e0 T start_cpu0
ffffffffb7e001f0 T __startup_64
)";

const char kRestrictedKallsyms[] = R"(
0000000000000000 A fixed_percpu_data
0000000000000000 A __per_cpu_start
0000000000000000 A cpu_debug_store
0000000000000000 A irq_stack_backing_store
0000000000000000 A cpu_tss_rw
0000000000000000 A gdt_page
0000000000000000 A entry_stack_storage
0000000000000000 A exception_stacks
0000000000000000 A espfix_stack
0000000000000000 A espfix_waddr
0000000000000000 A cpu_llc_id
0000000000000000 A mce_banks_array
0000000000000000 A mce_num_banks
0000000000000000 A cpu_sibling_map
0000000000000000 A cpu_core_map
0000000000000000 A cpu_die_map
0000000000000000 A cpu_info
0000000000000000 A cpu_llc_shared_map
0000000000000000 A cpu_number
0000000000000000 A this_cpu_off
0000000000000000 A x86_cpu_to_apicid
0000000000000000 A x86_bios_cpu_apicid
0000000000000000 A x86_cpu_to_acpiid
0000000000000000 A sched_core_priority
0000000000000000 A svm_data
0000000000000000 A current_tsc_ratio
0000000000000000 A saved_epb
0000000000000000 A cluster_masks
0000000000000000 A x86_cpu_to_logical_apicid
0000000000000000 A ipi_mask
0000000000000000 A menu_devices
0000000000000000 A cpu_loops_per_jiffy
0000000000000000 A cpu_hw_events
0000000000000000 A pmc_prev_left
0000000000000000 A perf_nmi_tstamp
0000000000000000 A bts_ctx
0000000000000000 A insn_buffer
0000000000000000 A pt_ctx
0000000000000000 A cpu_tsc_khz
0000000000000000 A current_vcpu
0000000000000000 A loaded_vmcss_on_cpu
0000000000000000 A current_vmcs
0000000000000000 A vmxarea
0000000000000000 A blocked_vcpu_on_cpu
0000000000000000 A blocked_vcpu_on_cpu_lock
0000000000000000 A irq_regs
0000000000000000 A nmi_state
0000000000000000 A nmi_cr2
0000000000000000 A update_debug_stack
0000000000000000 A last_nmi_rip
0000000000000000 A nmi_stats
0000000000000000 A swallow_nmi
0000000000000000 A vector_irq
0000000000000000 A processor_device_array
0000000000000000 A acpi_cpuidle_device
0000000000000000 A acpi_cstate
0000000000000000 A cpufreq_thermal_reduction_pctg
0000000000000000 A cpc_desc_ptr
0000000000000000 A cpu_pcc_subspace_idx
0000000000000000 A irq_randomness
0000000000000000 A batched_entropy_u64
0000000000000000 A batched_entropy_u32
0000000000000000 A drm_unplug_srcu_srcu_data
0000000000000000 A device_links_srcu_srcu_data
0000000000000000 A cpu_sys_devices
0000000000000000 A ci_cpu_cacheinfo
0000000000000000 A ci_cache_dev
0000000000000000 A ci_index_dev
0000000000000000 A wakeup_srcu_srcu_data
0000000000000000 A flush_idx
0000000000000000 A dax_srcu_srcu_data
0000000000000000 A cpufreq_cpu_data
0000000000000000 A cpufreq_transition_notifier_list_head_srcu_data
0000000000000000 A cpu_dbs
0000000000000000 A cpuidle_devices
0000000000000000 A cpuidle_dev
0000000000000000 A netdev_alloc_cache
0000000000000000 A napi_alloc_cache
0000000000000000 A flush_works
0000000000000000 A bpf_redirect_info
0000000000000000 A bpf_sp
0000000000000000 A nf_skb_duplicated
0000000000000000 A xt_recseq
0000000000000000 A rt_cache_stat
0000000000000000 A tsq_tasklet
0000000000000000 A xfrm_trans_tasklet
0000000000000000 A radix_tree_preloads
0000000000000000 A irq_stat
0000000000000000 A cyc2ns
0000000000000000 A cpu_tlbstate
0000000000000000 A flush_tlb_info
0000000000000000 A cpu_worker_pools
0000000000000000 A runqueues
0000000000000000 A sched_clock_data
0000000000000000 A osq_node
0000000000000000 A qnodes
0000000000000000 A rcu_data
0000000000000000 A cfd_data
0000000000000000 A call_single_queue
0000000000000000 A csd_data
0000000000000000 A softnet_data
0000000000000000 A rt_uncached_list
0000000000000000 A rt6_uncached_list
0000000000000000 A __per_cpu_end
0000000000000000 T startup_64
0000000000000000 T _stext
0000000000000000 T _text
0000000000000000 T secondary_startup_64
0000000000000000 T verify_cpu
0000000000000000 T start_cpu0
0000000000000000 T __startup_64
)";

TEST(LazyKernelSymbolizerTest, CanReadKernelSymbolAddresses) {
  {
    base::TempFile tmp = base::TempFile::Create();
    base::WriteAll(tmp.fd(), kRestrictedKallsyms, sizeof(kRestrictedKallsyms));
    base::FlushFile(tmp.fd());
    EXPECT_FALSE(
        LazyKernelSymbolizer::CanReadKernelSymbolAddresses(tmp.path().c_str()));
  }

  {
    base::TempFile tmp = base::TempFile::Create();
    base::WriteAll(tmp.fd(), kUnrestrictedKallsyms,
                   sizeof(kUnrestrictedKallsyms));
    base::FlushFile(tmp.fd());
    EXPECT_TRUE(
        LazyKernelSymbolizer::CanReadKernelSymbolAddresses(tmp.path().c_str()));
  }
}

}  // namespace
}  // namespace perfetto
