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
#include "perfetto/base/scoped_file.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "perfetto/tracing/core/trace_writer.h"

#include "perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

void ScanFilesDFS(
    const std::string& root_directory,
    const std::function<bool(BlockDeviceID block_device_id,
                             Inode inode_number,
                             const std::string& path,
                             protos::pbzero::InodeFileMap_Entry_Type type)>&
        fn) {
  std::vector<std::string> queue{root_directory};
  while (!queue.empty()) {
    struct dirent* entry;
    std::string directory = queue.back();
    queue.pop_back();
    base::ScopedDir dir(opendir(directory.c_str()));
    directory += "/";
    if (!dir)
      continue;
    while ((entry = readdir(dir.get())) != nullptr) {
      std::string filename = entry->d_name;
      if (filename == "." || filename == "..")
        continue;
      std::string filepath = directory + filename;

      struct stat buf;
      if (lstat(filepath.c_str(), &buf) != 0)
        continue;

      // This might happen on filesystems that do not return
      // information in entry->d_type.
      if (S_ISLNK(buf.st_mode))
        continue;

      Inode inode_number = entry->d_ino;
      BlockDeviceID block_device_id = buf.st_dev;

      protos::pbzero::InodeFileMap_Entry_Type type =
          protos::pbzero::InodeFileMap_Entry_Type_UNKNOWN;
      // Readdir and stat not guaranteed to have directory info for all systems
      if (entry->d_type == DT_DIR || S_ISDIR(buf.st_mode)) {
        // Continue iterating through files if current entry is a directory
        queue.push_back(filepath);
        type = protos::pbzero::InodeFileMap_Entry_Type_DIRECTORY;
      } else if (entry->d_type == DT_REG || S_ISREG(buf.st_mode)) {
        type = protos::pbzero::InodeFileMap_Entry_Type_FILE;
      }

      if (!fn(block_device_id, inode_number, filepath, type))
        return;
    }
  }
}

void CreateStaticDeviceToInodeMap(
    const std::string& root_directory,
    std::map<BlockDeviceID, std::map<Inode, InodeMapValue>>* static_file_map) {
  ScanFilesDFS(root_directory,
               [static_file_map](BlockDeviceID block_device_id,
                                 Inode inode_number, const std::string& path,
                                 protos::pbzero::InodeFileMap_Entry_Type type) {
                 std::map<Inode, InodeMapValue>& inode_map =
                     (*static_file_map)[block_device_id];
                 inode_map[inode_number].SetType(type);
                 inode_map[inode_number].AddPath(path);
                 return true;
               });
}

void FillInodeEntry(InodeFileMap* destination,
                    Inode inode_number,
                    const InodeMapValue& inode_map_value) {
  auto* entry = destination->add_entries();
  entry->set_inode_number(inode_number);
  entry->set_type(inode_map_value.type());
  for (const auto& path : inode_map_value.paths())
    entry->add_paths(path.c_str());
}

InodeFileDataSource::InodeFileDataSource(
    TracingSessionID id,
    std::map<BlockDeviceID, std::map<Inode, InodeMapValue>>* static_file_map,
    LRUInodeCache* cache,
    std::unique_ptr<TraceWriter> writer)
    : session_id_(id),
      static_file_map_(static_file_map),
      cache_(cache),
      writer_(std::move(writer)),
      weak_factory_(this) {}

void InodeFileDataSource::AddInodesFromFilesystemScan(
    const std::string& root_directory,
    BlockDeviceID provided_block_device_id,
    std::set<Inode>* inode_numbers,
    LRUInodeCache* cache,
    InodeFileMap* destination) {
  if (inode_numbers->empty())
    return;
  ScanFilesDFS(
      root_directory,
      [provided_block_device_id, inode_numbers, cache, destination](
          BlockDeviceID block_device_id, Inode inode_number,
          const std::string& path,
          protos::pbzero::InodeFileMap_Entry_Type type) {
        if (provided_block_device_id != block_device_id)
          return true;
        if (inode_numbers->find(inode_number) == inode_numbers->end())
          return true;
        std::pair<BlockDeviceID, Inode> key{block_device_id, inode_number};
        auto cur_val = cache->Get(key);
        if (cur_val != nullptr) {
          cur_val->AddPath(path);
          FillInodeEntry(destination, inode_number, *cur_val);
        } else {
          InodeMapValue new_val(InodeMapValue(type, {path}));
          cache->Insert(key, new_val);
          FillInodeEntry(destination, inode_number, new_val);
        }
        inode_numbers->erase(inode_number);
        if (inode_numbers->empty())
          return false;
        return true;
      });

  // Could not be found, just add the inode number
  PERFETTO_DLOG("%zu inodes not found", inode_numbers->size());
  for (const auto& unresolved_inode : *inode_numbers) {
    auto* entry = destination->add_entries();
    entry->set_inode_number(unresolved_inode);
  }
}

void InodeFileDataSource::AddInodesFromStaticMap(BlockDeviceID block_device_id,
                                                 std::set<Inode>* inode_numbers,
                                                 InodeFileMap* destination) {
  // Check if block device id exists in static file map
  auto static_map_entry = static_file_map_->find(block_device_id);
  if (static_map_entry == static_file_map_->end())
    return;

  uint64_t system_found_count = 0;
  for (auto it = inode_numbers->begin(); it != inode_numbers->end();) {
    Inode inode_number = *it;
    // Check if inode number exists in static file map for given block device id
    auto inode_it = static_map_entry->second.find(inode_number);
    if (inode_it == static_map_entry->second.end()) {
      ++it;
      continue;
    }
    system_found_count++;
    it = inode_numbers->erase(it);
    FillInodeEntry(destination, inode_number, inode_it->second);
  }
  PERFETTO_DLOG("%" PRIu64 " inodes found in static file map",
                system_found_count);
}

void InodeFileDataSource::AddInodesFromLRUCache(BlockDeviceID block_device_id,
                                                std::set<Inode>* inode_numbers,
                                                InodeFileMap* destination) {
  uint64_t cache_found_count = 0;
  for (auto it = inode_numbers->begin(); it != inode_numbers->end();) {
    Inode inode_number = *it;
    auto value = cache_->Get(std::make_pair(block_device_id, inode_number));
    if (value == nullptr) {
      ++it;
      continue;
    }
    cache_found_count++;
    it = inode_numbers->erase(it);
    FillInodeEntry(destination, inode_number, *value);
  }
  PERFETTO_DLOG("%" PRIu64 " inodes found in cache", cache_found_count);
}

void InodeFileDataSource::OnInodes(
    const std::vector<std::pair<Inode, BlockDeviceID>>& inodes) {
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
  PERFETTO_DLOG("Saw %zu block devices.", inode_file_maps.size());

  // Write a TracePacket with an InodeFileMap proto for each block device id
  for (const auto& inode_file_map_data : inode_file_maps) {
    BlockDeviceID block_device_id = inode_file_map_data.first;
    std::set<Inode> inode_numbers = inode_file_map_data.second;
    PERFETTO_DLOG("Saw %zu unique inode numbers.", inode_numbers.size());

    // New TracePacket for each InodeFileMap
    auto trace_packet = writer_->NewTracePacket();
    auto inode_file_map = trace_packet->set_inode_file_map();

    // Add block device id to InodeFileMap
    inode_file_map->set_block_device_id(block_device_id);

    // Add mount points to InodeFileMap
    auto range = mount_points_.equal_range(block_device_id);
    for (std::multimap<BlockDeviceID, std::string>::iterator it = range.first;
         it != range.second; ++it)
      inode_file_map->add_mount_points(it->second.c_str());

    // Add entries to InodeFileMap as inodes are found and resolved to their
    // paths/type
    AddInodesFromStaticMap(block_device_id, &inode_numbers, inode_file_map);
    AddInodesFromLRUCache(block_device_id, &inode_numbers, inode_file_map);
    // TODO(azappone): Make root directory a mount point
    std::string root_directory = "/data";
    AddInodesFromFilesystemScan(root_directory, block_device_id, &inode_numbers,
                                cache_, inode_file_map);
    trace_packet->Finalize();
  }
}

base::WeakPtr<InodeFileDataSource> InodeFileDataSource::GetWeakPtr() const {
  return weak_factory_.GetWeakPtr();
}

}  // namespace perfetto
