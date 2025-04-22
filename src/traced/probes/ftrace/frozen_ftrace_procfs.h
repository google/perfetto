/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef SRC_TRACED_PROBES_FTRACE_FROZEN_FTRACE_PROCFS_H_
#define SRC_TRACED_PROBES_FTRACE_FROZEN_FTRACE_PROCFS_H_

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "src/traced/probes/ftrace/ftrace_procfs.h"

namespace perfetto {

class FrozenFtraceProcfs : public FtraceProcfs {
 public:
  // Tries creating an |FrozenFtraceProcfs| at a persistent ring buffer
  // instance under the standard tracefs mount points. This requires
  // |instance_name| because the persistent ring buffer in the kernel
  // must be an instance. This also requires |event_format_path| such
  // as "/data/local/tmp/frozen_events/" which stores the format files
  // for events saved in the previous boot.
  static std::unique_ptr<FrozenFtraceProcfs> CreateGuessingMountPoint(
      const std::string& instance_name,
      const std::string& event_format_path);

  explicit FrozenFtraceProcfs(const std::string& root,
                              const std::string& event_format_path);
  virtual ~FrozenFtraceProcfs() override;

  // Read the format for event with the given |group| and |name| from
  // the previous boot event format file.
  virtual std::string ReadEventFormat(const std::string& group,
                                      const std::string& name) const override;

  // Get the clock name which is used in the previous boot.
  // Currently it is not supported, so return "boot" always.
  // TODO: b/411014640
  virtual std::string GetClock() override { return "boot"; }

 protected:
  // Frozen ftrace instance should not change anything (read only.)
  virtual bool WriteToFile(const std::string&, const std::string&) override {
    return false;
  }
  virtual bool AppendToFile(const std::string&, const std::string&) override {
    return false;
  }
  virtual bool ClearFile(const std::string&) override { return false; }
  virtual bool IsFileWriteable(const std::string&) override { return false; }

  static bool CheckFrozenPath(const std::string& root);
  const std::string event_format_path_;
};

}  // namespace perfetto
#endif  // SRC_TRACED_PROBES_FTRACE_FROZEN_FTRACE_PROCFS_H_
