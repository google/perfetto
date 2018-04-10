/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "tools/trace_to_text/ftrace_inode_handler.h"

namespace perfetto {

bool ParseInode(const protos::FtraceEvent& event, uint64_t* inode) {
  if (event.has_ext4_alloc_da_blocks() && event.ext4_alloc_da_blocks().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_alloc_da_blocks().ino());
    return true;
  } else if (event.has_ext4_allocate_blocks() &&
             event.ext4_allocate_blocks().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_allocate_blocks().ino());
    return true;
  } else if (event.has_ext4_allocate_inode() &&
             event.ext4_allocate_inode().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_allocate_inode().ino());
    return true;
  } else if (event.has_ext4_begin_ordered_truncate() &&
             event.ext4_begin_ordered_truncate().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_begin_ordered_truncate().ino());
    return true;
  } else if (event.has_ext4_collapse_range() &&
             event.ext4_collapse_range().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_collapse_range().ino());
    return true;
  } else if (event.has_ext4_da_release_space() &&
             event.ext4_da_release_space().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_da_release_space().ino());
    return true;
  } else if (event.has_ext4_da_reserve_space() &&
             event.ext4_da_reserve_space().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_da_reserve_space().ino());
    return true;
  } else if (event.has_ext4_da_update_reserve_space() &&
             event.ext4_da_update_reserve_space().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_da_update_reserve_space().ino());
    return true;
  } else if (event.has_ext4_da_write_begin() &&
             event.ext4_da_write_begin().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_da_write_begin().ino());
    return true;
  } else if (event.has_ext4_da_write_end() && event.ext4_da_write_end().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_da_write_end().ino());
    return true;
  } else if (event.has_ext4_da_write_pages() &&
             event.ext4_da_write_pages().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_da_write_pages().ino());
    return true;
  } else if (event.has_ext4_da_write_pages_extent() &&
             event.ext4_da_write_pages_extent().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_da_write_pages_extent().ino());
    return true;
  } else if (event.has_ext4_direct_io_enter() &&
             event.ext4_direct_io_enter().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_direct_io_enter().ino());
    return true;
  } else if (event.has_ext4_direct_io_exit() &&
             event.ext4_direct_io_exit().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_direct_io_exit().ino());
    return true;
  } else if (event.has_ext4_discard_preallocations() &&
             event.ext4_discard_preallocations().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_discard_preallocations().ino());
    return true;
  } else if (event.has_ext4_drop_inode() && event.ext4_drop_inode().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_drop_inode().ino());
    return true;
  } else if (event.has_ext4_es_cache_extent() &&
             event.ext4_es_cache_extent().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_es_cache_extent().ino());
    return true;
  } else if (event.has_ext4_es_find_delayed_extent_range_enter() &&
             event.ext4_es_find_delayed_extent_range_enter().ino()) {
    *inode = static_cast<uint64_t>(
        event.ext4_es_find_delayed_extent_range_enter().ino());
    return true;
  } else if (event.has_ext4_es_find_delayed_extent_range_exit() &&
             event.ext4_es_find_delayed_extent_range_exit().ino()) {
    *inode = static_cast<uint64_t>(
        event.ext4_es_find_delayed_extent_range_exit().ino());
    return true;
  } else if (event.has_ext4_es_insert_extent() &&
             event.ext4_es_insert_extent().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_es_insert_extent().ino());
    return true;
  } else if (event.has_ext4_es_lookup_extent_enter() &&
             event.ext4_es_lookup_extent_enter().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_es_lookup_extent_enter().ino());
    return true;
  } else if (event.has_ext4_es_lookup_extent_exit() &&
             event.ext4_es_lookup_extent_exit().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_es_lookup_extent_exit().ino());
    return true;
  } else if (event.has_ext4_es_remove_extent() &&
             event.ext4_es_remove_extent().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_es_remove_extent().ino());
    return true;
  } else if (event.has_ext4_evict_inode() && event.ext4_evict_inode().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_evict_inode().ino());
    return true;
  } else if (event.has_ext4_ext_convert_to_initialized_enter() &&
             event.ext4_ext_convert_to_initialized_enter().ino()) {
    *inode = static_cast<uint64_t>(
        event.ext4_ext_convert_to_initialized_enter().ino());
    return true;
  } else if (event.has_ext4_ext_convert_to_initialized_fastpath() &&
             event.ext4_ext_convert_to_initialized_fastpath().ino()) {
    *inode = static_cast<uint64_t>(
        event.ext4_ext_convert_to_initialized_fastpath().ino());
    return true;
  } else if (event.has_ext4_ext_handle_unwritten_extents() &&
             event.ext4_ext_handle_unwritten_extents().ino()) {
    *inode =
        static_cast<uint64_t>(event.ext4_ext_handle_unwritten_extents().ino());
    return true;
  } else if (event.has_ext4_ext_in_cache() && event.ext4_ext_in_cache().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_ext_in_cache().ino());
    return true;
  } else if (event.has_ext4_ext_load_extent() &&
             event.ext4_ext_load_extent().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_ext_load_extent().ino());
    return true;
  } else if (event.has_ext4_ext_map_blocks_enter() &&
             event.ext4_ext_map_blocks_enter().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_ext_map_blocks_enter().ino());
    return true;
  } else if (event.has_ext4_ext_map_blocks_exit() &&
             event.ext4_ext_map_blocks_exit().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_ext_map_blocks_exit().ino());
    return true;
  } else if (event.has_ext4_ext_put_in_cache() &&
             event.ext4_ext_put_in_cache().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_ext_put_in_cache().ino());
    return true;
  } else if (event.has_ext4_ext_remove_space() &&
             event.ext4_ext_remove_space().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_ext_remove_space().ino());
    return true;
  } else if (event.has_ext4_ext_remove_space_done() &&
             event.ext4_ext_remove_space_done().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_ext_remove_space_done().ino());
    return true;
  } else if (event.has_ext4_ext_rm_idx() && event.ext4_ext_rm_idx().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_ext_rm_idx().ino());
    return true;
  } else if (event.has_ext4_ext_rm_leaf() && event.ext4_ext_rm_leaf().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_ext_rm_leaf().ino());
    return true;
  } else if (event.has_ext4_ext_show_extent() &&
             event.ext4_ext_show_extent().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_ext_show_extent().ino());
    return true;
  } else if (event.has_ext4_fallocate_enter() &&
             event.ext4_fallocate_enter().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_fallocate_enter().ino());
    return true;
  } else if (event.has_ext4_fallocate_exit() &&
             event.ext4_fallocate_exit().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_fallocate_exit().ino());
    return true;
  } else if (event.has_ext4_find_delalloc_range() &&
             event.ext4_find_delalloc_range().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_find_delalloc_range().ino());
    return true;
  } else if (event.has_ext4_forget() && event.ext4_forget().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_forget().ino());
    return true;
  } else if (event.has_ext4_free_blocks() && event.ext4_free_blocks().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_free_blocks().ino());
    return true;
  } else if (event.has_ext4_free_inode() && event.ext4_free_inode().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_free_inode().ino());
    return true;
  } else if (event.has_ext4_get_reserved_cluster_alloc() &&
             event.ext4_get_reserved_cluster_alloc().ino()) {
    *inode =
        static_cast<uint64_t>(event.ext4_get_reserved_cluster_alloc().ino());
    return true;
  } else if (event.has_ext4_ind_map_blocks_enter() &&
             event.ext4_ind_map_blocks_enter().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_ind_map_blocks_enter().ino());
    return true;
  } else if (event.has_ext4_ind_map_blocks_exit() &&
             event.ext4_ind_map_blocks_exit().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_ind_map_blocks_exit().ino());
    return true;
  } else if (event.has_ext4_insert_range() && event.ext4_insert_range().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_insert_range().ino());
    return true;
  } else if (event.has_ext4_invalidatepage() &&
             event.ext4_invalidatepage().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_invalidatepage().ino());
    return true;
  } else if (event.has_ext4_journalled_invalidatepage() &&
             event.ext4_journalled_invalidatepage().ino()) {
    *inode =
        static_cast<uint64_t>(event.ext4_journalled_invalidatepage().ino());
    return true;
  } else if (event.has_ext4_journalled_write_end() &&
             event.ext4_journalled_write_end().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_journalled_write_end().ino());
    return true;
  } else if (event.has_ext4_load_inode() && event.ext4_load_inode().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_load_inode().ino());
    return true;
  } else if (event.has_ext4_mark_inode_dirty() &&
             event.ext4_mark_inode_dirty().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_mark_inode_dirty().ino());
    return true;
  } else if (event.has_ext4_mb_new_group_pa() &&
             event.ext4_mb_new_group_pa().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_mb_new_group_pa().ino());
    return true;
  } else if (event.has_ext4_mb_new_inode_pa() &&
             event.ext4_mb_new_inode_pa().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_mb_new_inode_pa().ino());
    return true;
  } else if (event.has_ext4_mb_release_inode_pa() &&
             event.ext4_mb_release_inode_pa().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_mb_release_inode_pa().ino());
    return true;
  } else if (event.has_ext4_mballoc_alloc() &&
             event.ext4_mballoc_alloc().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_mballoc_alloc().ino());
    return true;
  } else if (event.has_ext4_mballoc_discard() &&
             event.ext4_mballoc_discard().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_mballoc_discard().ino());
    return true;
  } else if (event.has_ext4_mballoc_free() && event.ext4_mballoc_free().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_mballoc_free().ino());
    return true;
  } else if (event.has_ext4_mballoc_prealloc() &&
             event.ext4_mballoc_prealloc().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_mballoc_prealloc().ino());
    return true;
  } else if (event.has_ext4_other_inode_update_time() &&
             event.ext4_other_inode_update_time().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_other_inode_update_time().ino());
    return true;
  } else if (event.has_ext4_other_inode_update_time() &&
             event.ext4_other_inode_update_time().orig_ino()) {
    *inode =
        static_cast<uint64_t>(event.ext4_other_inode_update_time().orig_ino());
    return true;
  } else if (event.has_ext4_punch_hole() && event.ext4_punch_hole().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_punch_hole().ino());
    return true;
  } else if (event.has_ext4_readpage() && event.ext4_readpage().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_readpage().ino());
    return true;
  } else if (event.has_ext4_releasepage() && event.ext4_releasepage().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_releasepage().ino());
    return true;
  } else if (event.has_ext4_remove_blocks() &&
             event.ext4_remove_blocks().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_remove_blocks().ino());
    return true;
  } else if (event.has_ext4_request_blocks() &&
             event.ext4_request_blocks().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_request_blocks().ino());
    return true;
  } else if (event.has_ext4_sync_file_enter() &&
             event.ext4_sync_file_enter().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_sync_file_enter().ino());
    return true;
  } else if (event.has_ext4_sync_file_exit() &&
             event.ext4_sync_file_exit().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_sync_file_exit().ino());
    return true;
  } else if (event.has_ext4_truncate_enter() &&
             event.ext4_truncate_enter().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_truncate_enter().ino());
    return true;
  } else if (event.has_ext4_truncate_exit() &&
             event.ext4_truncate_exit().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_truncate_exit().ino());
    return true;
  } else if (event.has_ext4_unlink_enter() && event.ext4_unlink_enter().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_unlink_enter().ino());
    return true;
  } else if (event.has_ext4_unlink_exit() && event.ext4_unlink_exit().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_unlink_exit().ino());
    return true;
  } else if (event.has_ext4_write_begin() && event.ext4_write_begin().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_write_begin().ino());
    return true;
  } else if (event.has_ext4_write_end() && event.ext4_write_end().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_write_end().ino());
    return true;
  } else if (event.has_ext4_writepage() && event.ext4_writepage().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_writepage().ino());
    return true;
  } else if (event.has_ext4_writepages() && event.ext4_writepages().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_writepages().ino());
    return true;
  } else if (event.has_ext4_writepages_result() &&
             event.ext4_writepages_result().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_writepages_result().ino());
    return true;
  } else if (event.has_ext4_zero_range() && event.ext4_zero_range().ino()) {
    *inode = static_cast<uint64_t>(event.ext4_zero_range().ino());
    return true;
  } else if (event.has_mm_filemap_add_to_page_cache() &&
             event.mm_filemap_add_to_page_cache().i_ino()) {
    *inode =
        static_cast<uint64_t>(event.mm_filemap_add_to_page_cache().i_ino());
    return true;
  } else if (event.has_mm_filemap_delete_from_page_cache() &&
             event.mm_filemap_delete_from_page_cache().i_ino()) {
    *inode = static_cast<uint64_t>(
        event.mm_filemap_delete_from_page_cache().i_ino());
    return true;
  } else if (event.has_f2fs_evict_inode() && event.f2fs_evict_inode().ino()) {
    *inode = static_cast<uint64_t>(event.f2fs_evict_inode().ino());
    return true;
  } else if (event.has_f2fs_evict_inode() && event.f2fs_evict_inode().pino()) {
    *inode = static_cast<uint64_t>(event.f2fs_evict_inode().pino());
    return true;
  } else if (event.has_f2fs_fallocate() && event.f2fs_fallocate().ino()) {
    *inode = static_cast<uint64_t>(event.f2fs_fallocate().ino());
    return true;
  } else if (event.has_f2fs_get_data_block() &&
             event.f2fs_get_data_block().ino()) {
    *inode = static_cast<uint64_t>(event.f2fs_get_data_block().ino());
    return true;
  } else if (event.has_f2fs_iget() && event.f2fs_iget().ino()) {
    *inode = static_cast<uint64_t>(event.f2fs_iget().ino());
    return true;
  } else if (event.has_f2fs_iget() && event.f2fs_iget().pino()) {
    *inode = static_cast<uint64_t>(event.f2fs_iget().pino());
    return true;
  } else if (event.has_f2fs_iget_exit() && event.f2fs_iget_exit().ino()) {
    *inode = static_cast<uint64_t>(event.f2fs_iget_exit().ino());
    return true;
  } else if (event.has_f2fs_new_inode() && event.f2fs_new_inode().ino()) {
    *inode = static_cast<uint64_t>(event.f2fs_new_inode().ino());
    return true;
  } else if (event.has_f2fs_readpage() && event.f2fs_readpage().ino()) {
    *inode = static_cast<uint64_t>(event.f2fs_readpage().ino());
    return true;
  } else if (event.has_f2fs_set_page_dirty() &&
             event.f2fs_set_page_dirty().ino()) {
    *inode = static_cast<uint64_t>(event.f2fs_set_page_dirty().ino());
    return true;
  } else if (event.has_f2fs_submit_write_page() &&
             event.f2fs_submit_write_page().ino()) {
    *inode = static_cast<uint64_t>(event.f2fs_submit_write_page().ino());
    return true;
  } else if (event.has_f2fs_sync_file_enter() &&
             event.f2fs_sync_file_enter().ino()) {
    *inode = static_cast<uint64_t>(event.f2fs_sync_file_enter().ino());
    return true;
  } else if (event.has_f2fs_sync_file_enter() &&
             event.f2fs_sync_file_enter().pino()) {
    *inode = static_cast<uint64_t>(event.f2fs_sync_file_enter().pino());
    return true;
  } else if (event.has_f2fs_sync_file_exit() &&
             event.f2fs_sync_file_exit().ino()) {
    *inode = static_cast<uint64_t>(event.f2fs_sync_file_exit().ino());
    return true;
  } else if (event.has_f2fs_truncate() && event.f2fs_truncate().ino()) {
    *inode = static_cast<uint64_t>(event.f2fs_truncate().ino());
    return true;
  } else if (event.has_f2fs_truncate() && event.f2fs_truncate().pino()) {
    *inode = static_cast<uint64_t>(event.f2fs_truncate().pino());
    return true;
  } else if (event.has_f2fs_truncate_blocks_enter() &&
             event.f2fs_truncate_blocks_enter().ino()) {
    *inode = static_cast<uint64_t>(event.f2fs_truncate_blocks_enter().ino());
    return true;
  } else if (event.has_f2fs_truncate_blocks_exit() &&
             event.f2fs_truncate_blocks_exit().ino()) {
    *inode = static_cast<uint64_t>(event.f2fs_truncate_blocks_exit().ino());
    return true;
  } else if (event.has_f2fs_truncate_data_blocks_range() &&
             event.f2fs_truncate_data_blocks_range().ino()) {
    *inode =
        static_cast<uint64_t>(event.f2fs_truncate_data_blocks_range().ino());
    return true;
  } else if (event.has_f2fs_truncate_inode_blocks_enter() &&
             event.f2fs_truncate_inode_blocks_enter().ino()) {
    *inode =
        static_cast<uint64_t>(event.f2fs_truncate_inode_blocks_enter().ino());
    return true;
  } else if (event.has_f2fs_truncate_inode_blocks_exit() &&
             event.f2fs_truncate_inode_blocks_exit().ino()) {
    *inode =
        static_cast<uint64_t>(event.f2fs_truncate_inode_blocks_exit().ino());
    return true;
  } else if (event.has_f2fs_truncate_node() &&
             event.f2fs_truncate_node().ino()) {
    *inode = static_cast<uint64_t>(event.f2fs_truncate_node().ino());
    return true;
  } else if (event.has_f2fs_truncate_nodes_enter() &&
             event.f2fs_truncate_nodes_enter().ino()) {
    *inode = static_cast<uint64_t>(event.f2fs_truncate_nodes_enter().ino());
    return true;
  } else if (event.has_f2fs_truncate_nodes_exit() &&
             event.f2fs_truncate_nodes_exit().ino()) {
    *inode = static_cast<uint64_t>(event.f2fs_truncate_nodes_exit().ino());
    return true;
  } else if (event.has_f2fs_truncate_partial_nodes() &&
             event.f2fs_truncate_partial_nodes().ino()) {
    *inode = static_cast<uint64_t>(event.f2fs_truncate_partial_nodes().ino());
    return true;
  } else if (event.has_f2fs_unlink_enter() && event.f2fs_unlink_enter().ino()) {
    *inode = static_cast<uint64_t>(event.f2fs_unlink_enter().ino());
    return true;
  } else if (event.has_f2fs_unlink_exit() && event.f2fs_unlink_exit().ino()) {
    *inode = static_cast<uint64_t>(event.f2fs_unlink_exit().ino());
    return true;
  } else if (event.has_f2fs_vm_page_mkwrite() &&
             event.f2fs_vm_page_mkwrite().ino()) {
    *inode = static_cast<uint64_t>(event.f2fs_vm_page_mkwrite().ino());
    return true;
  } else if (event.has_f2fs_write_begin() && event.f2fs_write_begin().ino()) {
    *inode = static_cast<uint64_t>(event.f2fs_write_begin().ino());
    return true;
  } else if (event.has_f2fs_write_end() && event.f2fs_write_end().ino()) {
    *inode = static_cast<uint64_t>(event.f2fs_write_end().ino());
    return true;
  }
  return false;
}

}  // namespace perfetto
