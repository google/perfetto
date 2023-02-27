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

#include "src/profiling/symbolizer/local_symbolizer.h"

#include <fcntl.h>

#include <cinttypes>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/profiling/symbolizer/elf.h"
#include "src/profiling/symbolizer/filesystem.h"
#include "src/profiling/symbolizer/scoped_read_mmap.h"

namespace perfetto {
namespace profiling {

// TODO(fmayer): Fix up name. This suggests it always returns a symbolizer or
// dies, which isn't the case.
std::unique_ptr<Symbolizer> LocalSymbolizerOrDie(
    std::vector<std::string> binary_path,
    const char* mode) {
  std::unique_ptr<Symbolizer> symbolizer;

  if (!binary_path.empty()) {
#if PERFETTO_BUILDFLAG(PERFETTO_LOCAL_SYMBOLIZER)
    std::unique_ptr<BinaryFinder> finder;
    if (!mode || strncmp(mode, "find", 4) == 0)
      finder.reset(new LocalBinaryFinder(std::move(binary_path)));
    else if (strncmp(mode, "index", 5) == 0)
      finder.reset(new LocalBinaryIndexer(std::move(binary_path)));
    else
      PERFETTO_FATAL("Invalid symbolizer mode [find | index]: %s", mode);
    symbolizer.reset(new LocalSymbolizer(std::move(finder)));
#else
    base::ignore_result(mode);
    PERFETTO_FATAL("This build does not support local symbolization.");
#endif
  }
  return symbolizer;
}

}  // namespace profiling
}  // namespace perfetto

#if PERFETTO_BUILDFLAG(PERFETTO_LOCAL_SYMBOLIZER)
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/utils.h"

#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
constexpr const char* kDefaultSymbolizer = "llvm-symbolizer.exe";
#else
constexpr const char* kDefaultSymbolizer = "llvm-symbolizer";
#endif

namespace perfetto {
namespace profiling {

std::vector<std::string> GetLines(
    std::function<int64_t(char*, size_t)> fn_read) {
  std::vector<std::string> lines;
  char buffer[512];
  int64_t rd = 0;
  // Cache the partial line of the previous read.
  std::string last_line;
  while ((rd = fn_read(buffer, sizeof(buffer))) > 0) {
    std::string data(buffer, static_cast<size_t>(rd));
    // Create stream buffer of last partial line + new data
    std::stringstream stream(last_line + data);
    std::string line;
    last_line = "";
    while (std::getline(stream, line)) {
      // Return from reading when we read an empty line.
      if (line.empty()) {
        return lines;
      } else if (stream.eof()) {
        // Cache off the partial line when we hit end of stream.
        last_line += line;
        break;
      } else {
        lines.push_back(line);
      }
    }
  }
  if (rd == -1) {
    PERFETTO_ELOG("Failed to read data from subprocess.");
  }
  return lines;
}

namespace {
bool InRange(const void* base,
             size_t total_size,
             const void* ptr,
             size_t size) {
  return ptr >= base && static_cast<const char*>(ptr) + size <=
                            static_cast<const char*>(base) + total_size;
}

template <typename E>
base::Optional<uint64_t> GetLoadBias(void* mem, size_t size) {
  const typename E::Ehdr* ehdr = static_cast<typename E::Ehdr*>(mem);
  if (!InRange(mem, size, ehdr, sizeof(typename E::Ehdr))) {
    PERFETTO_ELOG("Corrupted ELF.");
    return base::nullopt;
  }
  for (size_t i = 0; i < ehdr->e_phnum; ++i) {
    typename E::Phdr* phdr = GetPhdr<E>(mem, ehdr, i);
    if (!InRange(mem, size, phdr, sizeof(typename E::Phdr))) {
      PERFETTO_ELOG("Corrupted ELF.");
      return base::nullopt;
    }
    if (phdr->p_type == PT_LOAD && phdr->p_flags & PF_X) {
      return phdr->p_vaddr - phdr->p_offset;
    }
  }
  return 0u;
}

template <typename E>
base::Optional<std::string> GetBuildId(void* mem, size_t size) {
  const typename E::Ehdr* ehdr = static_cast<typename E::Ehdr*>(mem);
  if (!InRange(mem, size, ehdr, sizeof(typename E::Ehdr))) {
    PERFETTO_ELOG("Corrupted ELF.");
    return base::nullopt;
  }
  for (size_t i = 0; i < ehdr->e_shnum; ++i) {
    typename E::Shdr* shdr = GetShdr<E>(mem, ehdr, i);
    if (!InRange(mem, size, shdr, sizeof(typename E::Shdr))) {
      PERFETTO_ELOG("Corrupted ELF.");
      return base::nullopt;
    }

    if (shdr->sh_type != SHT_NOTE)
      continue;

    auto offset = shdr->sh_offset;
    while (offset < shdr->sh_offset + shdr->sh_size) {
      typename E::Nhdr* nhdr =
          reinterpret_cast<typename E::Nhdr*>(static_cast<char*>(mem) + offset);

      if (!InRange(mem, size, nhdr, sizeof(typename E::Nhdr))) {
        PERFETTO_ELOG("Corrupted ELF.");
        return base::nullopt;
      }
      if (nhdr->n_type == NT_GNU_BUILD_ID && nhdr->n_namesz == 4) {
        char* name = reinterpret_cast<char*>(nhdr) + sizeof(*nhdr);
        if (!InRange(mem, size, name, 4)) {
          PERFETTO_ELOG("Corrupted ELF.");
          return base::nullopt;
        }
        if (memcmp(name, "GNU", 3) == 0) {
          const char* value = reinterpret_cast<char*>(nhdr) + sizeof(*nhdr) +
                              base::AlignUp<4>(nhdr->n_namesz);

          if (!InRange(mem, size, value, nhdr->n_descsz)) {
            PERFETTO_ELOG("Corrupted ELF.");
            return base::nullopt;
          }
          return std::string(value, nhdr->n_descsz);
        }
      }
      offset += sizeof(*nhdr) + base::AlignUp<4>(nhdr->n_namesz) +
                base::AlignUp<4>(nhdr->n_descsz);
    }
  }
  return base::nullopt;
}

std::string SplitBuildID(const std::string& hex_build_id) {
  if (hex_build_id.size() < 3) {
    PERFETTO_DFATAL_OR_ELOG("Invalid build-id (< 3 char) %s",
                            hex_build_id.c_str());
    return {};
  }

  return hex_build_id.substr(0, 2) + "/" + hex_build_id.substr(2);
}

bool IsElf(const char* mem, size_t size) {
  if (size <= EI_MAG3)
    return false;
  return (mem[EI_MAG0] == ELFMAG0 && mem[EI_MAG1] == ELFMAG1 &&
          mem[EI_MAG2] == ELFMAG2 && mem[EI_MAG3] == ELFMAG3);
}

struct BuildIdAndLoadBias {
  std::string build_id;
  uint64_t load_bias;
};

base::Optional<BuildIdAndLoadBias> GetBuildIdAndLoadBias(const char* fname,
                                                         size_t size) {
  static_assert(EI_CLASS > EI_MAG3, "mem[EI_MAG?] accesses are in range.");
  if (size <= EI_CLASS)
    return base::nullopt;
  ScopedReadMmap map(fname, size);
  if (!map.IsValid()) {
    PERFETTO_PLOG("mmap");
    return base::nullopt;
  }
  char* mem = static_cast<char*>(*map);

  if (!IsElf(mem, size))
    return base::nullopt;

  base::Optional<std::string> build_id;
  base::Optional<uint64_t> load_bias;
  switch (mem[EI_CLASS]) {
    case ELFCLASS32:
      build_id = GetBuildId<Elf32>(mem, size);
      load_bias = GetLoadBias<Elf32>(mem, size);
      break;
    case ELFCLASS64:
      build_id = GetBuildId<Elf64>(mem, size);
      load_bias = GetLoadBias<Elf64>(mem, size);
      break;
    default:
      return base::nullopt;
  }
  if (build_id && load_bias) {
    return BuildIdAndLoadBias{*build_id, *load_bias};
  }
  return base::nullopt;
}

std::map<std::string, FoundBinary> BuildIdIndex(std::vector<std::string> dirs) {
  std::map<std::string, FoundBinary> result;
  WalkDirectories(std::move(dirs), [&result](const char* fname, size_t size) {
    char magic[EI_MAG3 + 1];
    // Scope file access. On windows OpenFile opens an exclusive lock.
    // This lock needs to be released before mapping the file.
    {
      base::ScopedFile fd(base::OpenFile(fname, O_RDONLY));
      if (!fd) {
        PERFETTO_PLOG("Failed to open %s", fname);
        return;
      }
      ssize_t rd = base::Read(*fd, &magic, sizeof(magic));
      if (rd != sizeof(magic)) {
        PERFETTO_PLOG("Failed to read %s", fname);
        return;
      }
      if (!IsElf(magic, static_cast<size_t>(rd))) {
        PERFETTO_DLOG("%s not an ELF.", fname);
        return;
      }
    }
    base::Optional<BuildIdAndLoadBias> build_id_and_load_bias =
        GetBuildIdAndLoadBias(fname, size);
    if (build_id_and_load_bias) {
      result.emplace(build_id_and_load_bias->build_id,
                     FoundBinary{fname, build_id_and_load_bias->load_bias});
    }
  });
  return result;
}

}  // namespace

bool ParseLlvmSymbolizerLine(const std::string& line,
                             std::string* file_name,
                             uint32_t* line_no) {
  size_t col_pos = line.rfind(':');
  if (col_pos == std::string::npos || col_pos == 0)
    return false;
  size_t row_pos = line.rfind(':', col_pos - 1);
  if (row_pos == std::string::npos || row_pos == 0)
    return false;
  *file_name = line.substr(0, row_pos);
  auto line_no_str = line.substr(row_pos + 1, col_pos - row_pos - 1);

  base::Optional<int32_t> opt_parsed_line_no = base::StringToInt32(line_no_str);
  if (!opt_parsed_line_no || *opt_parsed_line_no < 0)
    return false;
  *line_no = static_cast<uint32_t>(*opt_parsed_line_no);
  return true;
}

BinaryFinder::~BinaryFinder() = default;

LocalBinaryIndexer::LocalBinaryIndexer(std::vector<std::string> roots)
    : buildid_to_file_(BuildIdIndex(std::move(roots))) {}

base::Optional<FoundBinary> LocalBinaryIndexer::FindBinary(
    const std::string& abspath,
    const std::string& build_id) {
  auto it = buildid_to_file_.find(build_id);
  if (it != buildid_to_file_.end())
    return it->second;
  PERFETTO_ELOG("Could not find Build ID: %s (file %s).",
                base::ToHex(build_id).c_str(), abspath.c_str());
  return base::nullopt;
}

LocalBinaryIndexer::~LocalBinaryIndexer() = default;

LocalBinaryFinder::LocalBinaryFinder(std::vector<std::string> roots)
    : roots_(std::move(roots)) {}

base::Optional<FoundBinary> LocalBinaryFinder::FindBinary(
    const std::string& abspath,
    const std::string& build_id) {
  auto p = cache_.emplace(abspath, base::nullopt);
  if (!p.second)
    return p.first->second;

  base::Optional<FoundBinary>& cache_entry = p.first->second;

  for (const std::string& root_str : roots_) {
    cache_entry = FindBinaryInRoot(root_str, abspath, build_id);
    if (cache_entry)
      return cache_entry;
  }
  PERFETTO_ELOG("Could not find %s (Build ID: %s).", abspath.c_str(),
                base::ToHex(build_id).c_str());
  return cache_entry;
}

base::Optional<FoundBinary> LocalBinaryFinder::IsCorrectFile(
    const std::string& symbol_file,
    const std::string& build_id) {
  if (!base::FileExists(symbol_file)) {
    return base::nullopt;
  }
  // Openfile opens the file with an exclusive lock on windows.
  size_t size = GetFileSize(symbol_file);

  if (size == 0) {
    return base::nullopt;
  }

  base::Optional<BuildIdAndLoadBias> build_id_and_load_bias =
      GetBuildIdAndLoadBias(symbol_file.c_str(), size);
  if (!build_id_and_load_bias)
    return base::nullopt;
  if (build_id_and_load_bias->build_id != build_id) {
    return base::nullopt;
  }
  return FoundBinary{symbol_file, build_id_and_load_bias->load_bias};
}

base::Optional<FoundBinary> LocalBinaryFinder::FindBinaryInRoot(
    const std::string& root_str,
    const std::string& abspath,
    const std::string& build_id) {
  constexpr char kApkPrefix[] = "base.apk!";

  std::string filename;
  std::string dirname;

  for (base::StringSplitter sp(abspath, '/'); sp.Next();) {
    if (!dirname.empty())
      dirname += "/";
    dirname += filename;
    filename = sp.cur_token();
  }

  // Return the first match for the following options:
  // * absolute path of library file relative to root.
  // * absolute path of library file relative to root, but with base.apk!
  //   removed from filename.
  // * only filename of library file relative to root.
  // * only filename of library file relative to root, but with base.apk!
  //   removed from filename.
  // * in the subdirectory .build-id: the first two hex digits of the build-id
  //   as subdirectory, then the rest of the hex digits, with ".debug"appended.
  //   See
  //   https://fedoraproject.org/wiki/RolandMcGrath/BuildID#Find_files_by_build_ID
  //
  // For example, "/system/lib/base.apk!foo.so" with build id abcd1234,
  // is looked for at
  // * $ROOT/system/lib/base.apk!foo.so
  // * $ROOT/system/lib/foo.so
  // * $ROOT/base.apk!foo.so
  // * $ROOT/foo.so
  // * $ROOT/.build-id/ab/cd1234.debug

  base::Optional<FoundBinary> result;

  std::string symbol_file = root_str + "/" + dirname + "/" + filename;
  result = IsCorrectFile(symbol_file, build_id);
  if (result) {
    return result;
  }

  if (base::StartsWith(filename, kApkPrefix)) {
    symbol_file = root_str + "/" + dirname + "/" +
                  filename.substr(sizeof(kApkPrefix) - 1);
    result = IsCorrectFile(symbol_file, build_id);
    if (result) {
      return result;
    }
  }

  symbol_file = root_str + "/" + filename;
  result = IsCorrectFile(symbol_file, build_id);
  if (result) {
    return result;
  }

  if (base::StartsWith(filename, kApkPrefix)) {
    symbol_file = root_str + "/" + filename.substr(sizeof(kApkPrefix) - 1);
    result = IsCorrectFile(symbol_file, build_id);
    if (result) {
      return result;
    }
  }

  std::string hex_build_id = base::ToHex(build_id.c_str(), build_id.size());
  std::string split_hex_build_id = SplitBuildID(hex_build_id);
  if (!split_hex_build_id.empty()) {
    symbol_file =
        root_str + "/" + ".build-id" + "/" + split_hex_build_id + ".debug";
    result = IsCorrectFile(symbol_file, build_id);
    if (result) {
      return result;
    }
  }

  return base::nullopt;
}

LocalBinaryFinder::~LocalBinaryFinder() = default;

LLVMSymbolizerProcess::LLVMSymbolizerProcess(const std::string& symbolizer_path)
    :
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
      subprocess_(symbolizer_path, {}) {
}
#else
      subprocess_(symbolizer_path, {"llvm-symbolizer"}) {
}
#endif

std::vector<SymbolizedFrame> LLVMSymbolizerProcess::Symbolize(
    const std::string& binary,
    uint64_t address) {
  std::vector<SymbolizedFrame> result;
  base::StackString<1024> buffer("\"%s\" 0x%" PRIx64 "\n", binary.c_str(),
                                 address);
  if (subprocess_.Write(buffer.c_str(), buffer.len()) < 0) {
    PERFETTO_ELOG("Failed to write to llvm-symbolizer.");
    return result;
  }
  auto lines = GetLines([&](char* read_buffer, size_t buffer_size) {
    return subprocess_.Read(read_buffer, buffer_size);
  });
  // llvm-symbolizer writes out records in the form of
  // Foo(Bar*)
  // foo.cc:123
  // This is why we should always get a multiple of two number of lines.
  PERFETTO_DCHECK(lines.size() % 2 == 0);
  result.resize(lines.size() / 2);
  for (size_t i = 0; i < lines.size(); ++i) {
    SymbolizedFrame& cur = result[i / 2];
    if (i % 2 == 0) {
      cur.function_name = lines[i];
    } else {
      if (!ParseLlvmSymbolizerLine(lines[i], &cur.file_name, &cur.line)) {
        PERFETTO_ELOG("Failed to parse llvm-symbolizer line: %s",
                      lines[i].c_str());
        cur.file_name = "";
        cur.line = 0;
      }
    }
  }

  for (auto it = result.begin(); it != result.end();) {
    if (it->function_name == "??")
      it = result.erase(it);
    else
      ++it;
  }
  return result;
}
std::vector<std::vector<SymbolizedFrame>> LocalSymbolizer::Symbolize(
    const std::string& mapping_name,
    const std::string& build_id,
    uint64_t load_bias,
    const std::vector<uint64_t>& addresses) {
  base::Optional<FoundBinary> binary =
      finder_->FindBinary(mapping_name, build_id);
  if (!binary)
    return {};
  uint64_t load_bias_correction = 0;
  if (binary->load_bias > load_bias) {
    // On Android 10, there was a bug in libunwindstack that would incorrectly
    // calculate the load_bias, and thus the relative PC. This would end up in
    // frames that made no sense. We can fix this up after the fact if we
    // detect this situation.
    load_bias_correction = binary->load_bias - load_bias;
    PERFETTO_LOG("Correcting load bias by %" PRIu64 " for %s",
                 load_bias_correction, mapping_name.c_str());
  }
  std::vector<std::vector<SymbolizedFrame>> result;
  result.reserve(addresses.size());
  for (uint64_t address : addresses)
    result.emplace_back(llvm_symbolizer_.Symbolize(
        binary->file_name, address + load_bias_correction));
  return result;
}

LocalSymbolizer::LocalSymbolizer(const std::string& symbolizer_path,
                                 std::unique_ptr<BinaryFinder> finder)
    : llvm_symbolizer_(symbolizer_path), finder_(std::move(finder)) {}

LocalSymbolizer::LocalSymbolizer(std::unique_ptr<BinaryFinder> finder)
    : LocalSymbolizer(kDefaultSymbolizer, std::move(finder)) {}

LocalSymbolizer::~LocalSymbolizer() = default;

}  // namespace profiling
}  // namespace perfetto

#endif  // PERFETTO_BUILDFLAG(PERFETTO_LOCAL_SYMBOLIZER)
