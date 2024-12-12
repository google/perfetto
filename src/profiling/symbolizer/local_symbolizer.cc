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

#include <charconv>
#include <cinttypes>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/scoped_mmap.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/profiling/symbolizer/binary_info.h"
#include "src/profiling/symbolizer/elf.h"
#include "src/profiling/symbolizer/filesystem.h"

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

namespace {

std::optional<BinaryInfo> ReadBinaryInfoFromFile(const char* fname) {
  if (!base::FileExists(fname)) {
    return std::nullopt;
  }
  // Openfile opens the file with an exclusive lock on windows.
  std::optional<uint64_t> file_size = base::GetFileSize(fname);
  if (!file_size.has_value()) {
    PERFETTO_PLOG("Failed to get file size %s", fname);
    return std::nullopt;
  }

  static_assert(sizeof(size_t) <= sizeof(uint64_t));
  size_t size = static_cast<size_t>(
      std::min<uint64_t>(std::numeric_limits<size_t>::max(), *file_size));

  if (size == 0) {
    return std::nullopt;
  }

  base::ScopedMmap map = base::ReadMmapFilePart(fname, size);
  if (!map.IsValid()) {
    PERFETTO_PLOG("Failed to mmap %s", fname);
    return std::nullopt;
  }
  const uint8_t* mem = static_cast<const uint8_t*>(map.data());

  return GetBinaryInfo(mem, size);
}

std::string GetLine(std::function<int64_t(char*, size_t)> fn_read) {
  std::string line;
  char buffer[512];
  int64_t rd = 0;
  while ((rd = fn_read(buffer, sizeof(buffer))) > 0) {
    std::string data(buffer, static_cast<size_t>(rd));
    line += data;
    if (line.back() == '\n') {
      break;
    }
    // There should be no intermediate new lines in the read data.
    PERFETTO_DCHECK(line.find('\n') == std::string::npos);
  }
  if (rd == -1) {
    PERFETTO_ELOG("Failed to read data from subprocess.");
  }
  return line;
}

std::string SplitBuildID(const std::string& hex_build_id) {
  if (hex_build_id.size() < 3) {
    PERFETTO_DFATAL_OR_ELOG("Invalid build-id (< 3 char) %s",
                            hex_build_id.c_str());
    return {};
  }

  return hex_build_id.substr(0, 2) + "/" + hex_build_id.substr(2);
}

std::map<std::string, FoundBinary> BuildIdIndex(std::vector<std::string> dirs) {
  std::map<std::string, FoundBinary> result;
  WalkDirectories(std::move(dirs), [&result](const char* fname, size_t) {
    std::optional<BinaryInfo> binary_info = ReadBinaryInfoFromFile(fname);
    if (!binary_info || !binary_info->build_id) {
      PERFETTO_DLOG("Failed to extract build id from %s.", fname);
      return;
    }
    auto it = result.emplace(
        *binary_info->build_id,
        FoundBinary{fname, binary_info->load_bias, binary_info->type});

    // If there was already an existing FoundBinary, the emplace wouldn't insert
    // anything. But, for Mac binaries, we prefer dSYM files over the original
    // binary, so make sure these overwrite the FoundBinary entry.
    bool has_existing = it.second == false;
    if (has_existing) {
      if (it.first->second.type == BinaryType::kMachO &&
          binary_info->type == BinaryType::kMachODsym) {
        PERFETTO_LOG("Overwriting index entry for %s to %s.",
                     base::ToHex(*binary_info->build_id).c_str(), fname);
        it.first->second =
            FoundBinary{fname, binary_info->load_bias, binary_info->type};
      } else {
        PERFETTO_DLOG("Ignoring %s, index entry for %s already exists.", fname,
                      base::ToHex(*binary_info->build_id).c_str());
      }
    } else {
      PERFETTO_LOG("Indexed: %s (%s)", fname,
                   base::ToHex(*binary_info->build_id).c_str());
    }
  });
  return result;
}

bool ParseJsonString(const char*& it, const char* end, std::string* out) {
  *out = "";
  if (it == end) {
    return false;
  }
  if (*it++ != '"') {
    return false;
  }
  while (true) {
    if (it == end) {
      return false;
    }
    char c = *it++;
    if (c == '"') {
      return true;
    }
    if (c == '\\') {
      if (it == end) {
        return false;
      }
      c = *it++;
      switch (c) {
        case '"':
        case '\\':
        case '/':
          out->push_back(c);
          break;
        case 'b':
          out->push_back('\b');
          break;
        case 'f':
          out->push_back('\f');
          break;
        case 'n':
          out->push_back('\n');
          break;
        case 'r':
          out->push_back('\r');
          break;
        case 't':
          out->push_back('\t');
          break;
        // Pass-through \u escape codes without re-encoding to utf-8, for
        // simplicity.
        case 'u':
          out->push_back('\\');
          out->push_back('u');
          break;
        default:
          return false;
      }
    } else {
      out->push_back(c);
    }
  }
}

bool ParseJsonNumber(const char*& it, const char* end, double* out) {
  bool is_minus = false;
  double ret = 0;
  if (it == end) {
    return false;
  }
  if (*it == '-') {
    ++it;
    is_minus = true;
  }
  while (true) {
    if (it == end) {
      return false;
    }
    char c = *it++;
    if (isdigit(c)) {
      ret = ret * 10 + (c - '0');
    } else if (c == 'e') {
      // Scientific syntax is not supported.
      return false;
    } else {
      // Unwind the iterator to point at the end of the number.
      it--;
      break;
    }
  }
  *out = is_minus ? -ret : ret;
  return true;
}

bool ParseJsonArray(
    const char*& it,
    const char* end,
    std::function<bool(const char*&, const char*)> process_value) {
  if (it == end) {
    return false;
  }
  char c = *it++;
  if (c != '[') {
    return false;
  }
  while (true) {
    if (!process_value(it, end)) {
      return false;
    }
    if (it == end) {
      return false;
    }
    c = *it++;
    if (c == ']') {
      return true;
    }
    if (c != ',') {
      return false;
    }
  }
}

bool ParseJsonObject(
    const char*& it,
    const char* end,
    std::function<bool(const char*&, const char*, const std::string&)>
        process_value) {
  if (it == end) {
    return false;
  }
  char c = *it++;
  if (c != '{') {
    return false;
  }
  while (true) {
    std::string key;
    if (!ParseJsonString(it, end, &key)) {
      return false;
    }
    if (*it++ != ':') {
      return false;
    }
    if (!process_value(it, end, key)) {
      return false;
    }
    if (it == end) {
      return false;
    }
    c = *it++;
    if (c == '}') {
      return true;
    }
    if (c != ',') {
      return false;
    }
  }
}

bool SkipJsonValue(const char*& it, const char* end) {
  if (it == end) {
    return false;
  }
  char c = *it;
  if (c == '"') {
    std::string ignored;
    return ParseJsonString(it, end, &ignored);
  }
  if (isdigit(c) || c == '-') {
    double ignored;
    return ParseJsonNumber(it, end, &ignored);
  }
  if (c == '[') {
    return ParseJsonArray(it, end, [](const char*& it, const char* end) {
      return SkipJsonValue(it, end);
    });
  }
  if (c == '{') {
    return ParseJsonObject(
        it, end, [](const char*& it, const char* end, const std::string&) {
          return SkipJsonValue(it, end);
        });
  }
  return false;
}

}  // namespace

bool ParseLlvmSymbolizerJsonLine(const std::string& line,
                                 std::vector<SymbolizedFrame>* result) {
  // Parse Json of the format:
  // ```
  // {"Address":"0x1b72f","ModuleName":"...","Symbol":[{"Column":0,
  // "Discriminator":0,"FileName":"...","FunctionName":"...","Line":0,
  // "StartAddress":"","StartFileName":"...","StartLine":0},...]}
  // ```
  const char* it = line.data();
  const char* end = it + line.size();
  return ParseJsonObject(
      it, end, [&](const char*& it, const char* end, const std::string& key) {
        if (key == "Symbol") {
          return ParseJsonArray(it, end, [&](const char*& it, const char* end) {
            SymbolizedFrame frame;
            if (!ParseJsonObject(
                    it, end,
                    [&](const char*& it, const char* end,
                        const std::string& key) {
                      if (key == "FileName") {
                        return ParseJsonString(it, end, &frame.file_name);
                      }
                      if (key == "FunctionName") {
                        return ParseJsonString(it, end, &frame.function_name);
                      }
                      if (key == "Line") {
                        double number;
                        if (!ParseJsonNumber(it, end, &number)) {
                          return false;
                        }
                        frame.line = static_cast<unsigned int>(number);
                        return true;
                      }
                      return SkipJsonValue(it, end);
                    })) {
              return false;
            }
            // Use "??" for empty filenames, to match non-JSON output.
            if (frame.file_name.empty()) {
              frame.file_name = "??";
            }
            result->push_back(frame);
            return true;
          });
        }
        if (key == "Error") {
          std::string message;
          if (!ParseJsonObject(it, end,
                               [&](const char*& it, const char* end,
                                   const std::string& key) {
                                 if (key == "Message") {
                                   return ParseJsonString(it, end, &message);
                                 }
                                 return SkipJsonValue(it, end);
                               })) {
            return false;
          }
          PERFETTO_ELOG("Failed to symbolize: %s.", message.c_str());
          return true;
        }
        return SkipJsonValue(it, end);
      });
}

BinaryFinder::~BinaryFinder() = default;

LocalBinaryIndexer::LocalBinaryIndexer(std::vector<std::string> roots)
    : buildid_to_file_(BuildIdIndex(std::move(roots))) {}

std::optional<FoundBinary> LocalBinaryIndexer::FindBinary(
    const std::string& abspath,
    const std::string& build_id) {
  auto it = buildid_to_file_.find(build_id);
  if (it != buildid_to_file_.end())
    return it->second;
  PERFETTO_ELOG("Could not find Build ID: %s (file %s).",
                base::ToHex(build_id).c_str(), abspath.c_str());
  return std::nullopt;
}

LocalBinaryIndexer::~LocalBinaryIndexer() = default;

LocalBinaryFinder::LocalBinaryFinder(std::vector<std::string> roots)
    : roots_(std::move(roots)) {}

std::optional<FoundBinary> LocalBinaryFinder::FindBinary(
    const std::string& abspath,
    const std::string& build_id) {
  auto p = cache_.emplace(abspath, std::nullopt);
  if (!p.second)
    return p.first->second;

  std::optional<FoundBinary>& cache_entry = p.first->second;

  // Try the absolute path first.
  if (base::StartsWith(abspath, "/")) {
    cache_entry = IsCorrectFile(abspath, build_id);
    if (cache_entry)
      return cache_entry;
  }

  for (const std::string& root_str : roots_) {
    cache_entry = FindBinaryInRoot(root_str, abspath, build_id);
    if (cache_entry)
      return cache_entry;
  }
  PERFETTO_ELOG("Could not find %s (Build ID: %s).", abspath.c_str(),
                base::ToHex(build_id).c_str());
  return cache_entry;
}

std::optional<FoundBinary> LocalBinaryFinder::IsCorrectFile(
    const std::string& symbol_file,
    const std::string& build_id) {
  std::optional<BinaryInfo> binary_info =
      ReadBinaryInfoFromFile(symbol_file.c_str());
  if (!binary_info)
    return std::nullopt;
  if (binary_info->build_id != build_id) {
    return std::nullopt;
  }
  return FoundBinary{symbol_file, binary_info->load_bias, binary_info->type};
}

std::optional<FoundBinary> LocalBinaryFinder::FindBinaryInRoot(
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

  std::optional<FoundBinary> result;

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

  return std::nullopt;
}

LocalBinaryFinder::~LocalBinaryFinder() = default;

LLVMSymbolizerProcess::LLVMSymbolizerProcess(const std::string& symbolizer_path)
    :
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
      subprocess_(symbolizer_path, {"--output-style=JSON"}) {
}
#else
      subprocess_(symbolizer_path, {"llvm-symbolizer", "--output-style=JSON"}) {
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
  auto line = GetLine([&](char* read_buffer, size_t buffer_size) {
    return subprocess_.Read(read_buffer, buffer_size);
  });
  // llvm-symbolizer writes out records as one JSON per line.
  if (!ParseLlvmSymbolizerJsonLine(line, &result)) {
    PERFETTO_ELOG("Failed to parse llvm-symbolizer JSON: %s", line.c_str());
    return {};
  }
  return result;
}
std::vector<std::vector<SymbolizedFrame>> LocalSymbolizer::Symbolize(
    const std::string& mapping_name,
    const std::string& build_id,
    uint64_t load_bias,
    const std::vector<uint64_t>& addresses) {
  std::optional<FoundBinary> binary =
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
