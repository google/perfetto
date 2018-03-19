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

#include "src/traced/probes/filesystem/inode_file_data_source.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <queue>

#include "perfetto/base/logging.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "perfetto/tracing/core/trace_writer.h"

#include "perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

void CreateDeviceToInodeMap(
    const std::string& root_directory,
    std::map<BlockDeviceID, std::map<Inode, InodeMapValue>>* block_device_map) {
  std::queue<std::string> queue;
  queue.push(root_directory);
  while (!queue.empty()) {
    struct dirent* entry;
    std::string filepath = queue.front();
    queue.pop();
    DIR* dir = opendir(filepath.c_str());
    filepath += "/";
    if (dir == nullptr)
      continue;
    while ((entry = readdir(dir)) != nullptr) {
      std::string filename = entry->d_name;
      if (filename == "." || filename == "..")
        continue;
      Inode inode_number = static_cast<Inode>(entry->d_ino);
      struct stat buf;
      if (lstat(filepath.c_str(), &buf) != 0)
        continue;
      BlockDeviceID block_device_id = buf.st_dev;
      // Default
      protos::pbzero::InodeFileMap_Entry_Type type =
          protos::pbzero::InodeFileMap_Entry_Type_UNKNOWN;
      // Readdir and stat not guaranteed to have directory info for all systems
      if (entry->d_type == DT_DIR || S_ISDIR(buf.st_mode)) {
        // Continue iterating through files if current entry is a directory
        queue.push(filepath + filename);
        type = protos::pbzero::InodeFileMap_Entry_Type_DIRECTORY;
      } else if (entry->d_type == DT_REG || S_ISREG(buf.st_mode)) {
        type = protos::pbzero::InodeFileMap_Entry_Type_FILE;
      }

      // Update map
      std::map<Inode, InodeMapValue>& inode_map =
          (*block_device_map)[block_device_id];
      inode_map[inode_number].SetType(type);
      inode_map[inode_number].AddPath(filepath + filename);
    }
    closedir(dir);
  }
}

InodeFileDataSource::InodeFileDataSource(
    TracingSessionID id,
    std::map<BlockDeviceID, std::map<Inode, InodeMapValue>>*
        system_partition_files,
    std::unique_ptr<TraceWriter> writer)
    : session_id_(id),
      system_partition_files_(system_partition_files),
      writer_(std::move(writer)),
      weak_factory_(this) {}

bool InodeFileDataSource::AddInodeFileMapEntry(
    InodeFileMap* inode_file_map,
    BlockDeviceID block_device_id,
    Inode inode_number,
    const std::map<BlockDeviceID, std::map<Inode, InodeMapValue>>&
        current_partition_map) {
  auto block_device_entry = current_partition_map.find(block_device_id);
  if (block_device_entry != current_partition_map.end()) {
    auto inode_map = block_device_entry->second.find(inode_number);
    if (inode_map != block_device_entry->second.end()) {
      auto* entry = inode_file_map->add_entries();
      entry->set_inode_number(inode_number);
      entry->set_type(inode_map->second.type());
      for (const auto& path : inode_map->second.paths())
        entry->add_paths(path.c_str());
      return true;
    }
  }
  return false;
}

void InodeFileDataSource::OnInodes(
    const std::vector<std::pair<Inode, BlockDeviceID>>& inodes) {
  PERFETTO_DLOG("Saw FtraceBundle with %zu inodes.", inodes.size());

  if (mount_points_.empty()) {
    mount_points_ = ParseMounts();
  }
  // Group inodes from FtraceMetadata by block device
  std::map<BlockDeviceID, std::set<Inode>> inode_file_maps;
  for (const auto& inodes_pair : inodes) {
    Inode inode_number = inodes_pair.first;
    BlockDeviceID block_device_id = inodes_pair.second;
    inode_file_maps[block_device_id].emplace(inode_number);
  }
  // Write a TracePacket with an InodeFileMap proto for each block device id
  for (const auto& inode_file_map_data : inode_file_maps) {
    BlockDeviceID block_device_id = inode_file_map_data.first;
    std::set<Inode> inode_numbers = inode_file_map_data.second;

    // New TracePacket for each InodeFileMap
    auto trace_packet = writer_->NewTracePacket();
    auto inode_file_map = trace_packet->set_inode_file_map();

    // Add block device id
    inode_file_map->set_block_device_id(block_device_id);

    // Add mount points
    auto range = mount_points_.equal_range(block_device_id);
    for (std::multimap<BlockDeviceID, std::string>::iterator it = range.first;
         it != range.second; ++it)
      inode_file_map->add_mount_points(it->second.c_str());

    // Add entries for inodes in system
    std::map<BlockDeviceID, std::set<Inode>> data_partition_inodes;
    for (const auto& inode_number : inode_numbers) {
      // Search in /system partition and add to InodeFileMap if found
      bool in_system =
          AddInodeFileMapEntry(inode_file_map, block_device_id, inode_number,
                               *system_partition_files_);
      if (!in_system) {
        // TODO(fmayer): Add LRU and check before adding inode for full scan
        data_partition_inodes[block_device_id].emplace(inode_number);
      }
    }

    trace_packet->Finalize();
  }
}

base::WeakPtr<InodeFileDataSource> InodeFileDataSource::GetWeakPtr() const {
  return weak_factory_.GetWeakPtr();
}

}  // namespace perfetto
