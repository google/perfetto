
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

#include "perfetto/base/build_config.h"

// This translation unit is built only on Linux and MacOS. See //gn/BUILD.gn.
#if PERFETTO_BUILDFLAG(PERFETTO_LOCAL_SYMBOLIZER)

#include "src/profiling/symbolizer/local_symbolizer.h"

#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/utils.h"

#include <inttypes.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace perfetto {
namespace profiling {

namespace {

std::vector<std::string> GetLines(FILE* f) {
  std::vector<std::string> lines;
  size_t n = 0;
  char* line = nullptr;
  ssize_t rd = 0;
  do {
    rd = getline(&line, &n, f);
    // Do not read empty line that terminates the output.
    if (rd > 1) {
      // Remove newline character.
      PERFETTO_DCHECK(line[rd - 1] == '\n');
      line[rd - 1] = '\0';
      lines.emplace_back(line);
    }
    free(line);
    line = nullptr;
    n = 0;
  } while (rd > 1);
  return lines;
}

// We cannot just include elf.h, as that only exists on Linux, and we want to
// allow symbolization on other platforms as well. As we only need a small
// subset, it is easiest to define the constants and structs ourselves.
constexpr auto SHT_NOTE = 7;
constexpr auto NT_GNU_BUILD_ID = 3;
constexpr auto ELFCLASS32 = 1;
constexpr auto ELFCLASS64 = 2;
constexpr auto ELFMAG0 = 0x7f;
constexpr auto ELFMAG1 = 'E';
constexpr auto ELFMAG2 = 'L';
constexpr auto ELFMAG3 = 'F';
constexpr auto EI_MAG0 = 0;
constexpr auto EI_MAG1 = 1;
constexpr auto EI_MAG2 = 2;
constexpr auto EI_MAG3 = 3;
constexpr auto EI_CLASS = 4;

struct Elf32 {
  using Addr = uint32_t;
  using Half = uint16_t;
  using Off = uint32_t;
  using Sword = int32_t;
  using Word = uint32_t;
  struct Ehdr {
    unsigned char e_ident[16];
    Half e_type;
    Half e_machine;
    Word e_version;
    Addr e_entry;
    Off e_phoff;
    Off e_shoff;
    Word e_flags;
    Half e_ehsize;
    Half e_phentsize;
    Half e_phnum;
    Half e_shentsize;
    Half e_shnum;
    Half e_shstrndx;
  };
  struct Shdr {
    Word sh_name;
    Word sh_type;
    Word sh_flags;
    Addr sh_addr;
    Off sh_offset;
    Word sh_size;
    Word sh_link;
    Word sh_info;
    Word sh_addralign;
    Word sh_entsize;
  };
  struct Nhdr {
    Word n_namesz;
    Word n_descsz;
    Word n_type;
  };
};

struct Elf64 {
  using Addr = uint64_t;
  using Half = uint16_t;
  using SHalf = int16_t;
  using Off = uint64_t;
  using Sword = int32_t;
  using Word = uint32_t;
  using Xword = uint64_t;
  using Sxword = int64_t;
  struct Ehdr {
    unsigned char e_ident[16];
    Half e_type;
    Half e_machine;
    Word e_version;
    Addr e_entry;
    Off e_phoff;
    Off e_shoff;
    Word e_flags;
    Half e_ehsize;
    Half e_phentsize;
    Half e_phnum;
    Half e_shentsize;
    Half e_shnum;
    Half e_shstrndx;
  };
  struct Shdr {
    Word sh_name;
    Word sh_type;
    Xword sh_flags;
    Addr sh_addr;
    Off sh_offset;
    Xword sh_size;
    Word sh_link;
    Word sh_info;
    Xword sh_addralign;
    Xword sh_entsize;
  };
  struct Nhdr {
    Word n_namesz;
    Word n_descsz;
    Word n_type;
  };
};

template <typename E>
typename E::Shdr* GetShdr(void* mem, const typename E::Ehdr* ehdr, size_t i) {
  return reinterpret_cast<typename E::Shdr*>(
      static_cast<char*>(mem) + ehdr->e_shoff + i * sizeof(typename E::Shdr));
}

bool InRange(const void* base,
             size_t total_size,
             const void* ptr,
             size_t size) {
  return ptr >= base && static_cast<const char*>(ptr) + size <=
                            static_cast<const char*>(base) + total_size;
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

class ScopedMmap {
 public:
  ScopedMmap(void* addr,
             size_t length,
             int prot,
             int flags,
             int fd,
             off_t offset)
      : length_(length), ptr_(mmap(addr, length, prot, flags, fd, offset)) {}
  ~ScopedMmap() {
    if (ptr_ != MAP_FAILED)
      munmap(ptr_, length_);
  }

  void* operator*() { return ptr_; }

 private:
  size_t length_;
  void* ptr_;
};

std::string SplitBuildID(const std::string& hex_build_id) {
  if (hex_build_id.size() < 3) {
    PERFETTO_DFATAL_OR_ELOG("Invalid build-id (< 3 char) %s",
                            hex_build_id.c_str());
    return {};
  }

  return hex_build_id.substr(0, 2) + "/" + hex_build_id.substr(2);
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

base::Optional<std::string> LocalBinaryFinder::FindBinary(
    const std::string& abspath,
    const std::string& build_id) {
  auto p = cache_.emplace(abspath, base::nullopt);
  if (!p.second)
    return p.first->second;

  base::Optional<std::string>& cache_entry = p.first->second;

  for (const std::string& root_str : roots_) {
    cache_entry = FindBinaryInRoot(root_str, abspath, build_id);
    if (cache_entry)
      return cache_entry;
  }
  PERFETTO_ELOG("Could not find %s (Build ID: %s).", abspath.c_str(),
                base::ToHex(build_id).c_str());
  return cache_entry;
}

bool LocalBinaryFinder::IsCorrectFile(const std::string& symbol_file,
                                      const std::string& build_id) {
  base::ScopedFile fd(base::OpenFile(symbol_file, O_RDONLY));
  if (!fd)
    return false;

  struct stat statbuf;
  if (fstat(*fd, &statbuf) == -1)
    return false;

  size_t size = static_cast<size_t>(statbuf.st_size);

  static_assert(EI_CLASS > EI_MAG3, "mem[EI_MAG?] accesses are in range.");
  if (size <= EI_CLASS)
    return false;

  ScopedMmap map(nullptr, size, PROT_READ, MAP_PRIVATE, *fd, 0);
  if (*map == MAP_FAILED) {
    PERFETTO_PLOG("mmap");
    return false;
  }
  char* mem = static_cast<char*>(*map);

  if (mem[EI_MAG0] != ELFMAG0 || mem[EI_MAG1] != ELFMAG1 ||
      mem[EI_MAG2] != ELFMAG2 || mem[EI_MAG3] != ELFMAG3) {
    return false;
  }

  switch (mem[EI_CLASS]) {
    case ELFCLASS32:
      return build_id == GetBuildId<Elf32>(mem, size);
    case ELFCLASS64:
      return build_id == GetBuildId<Elf64>(mem, size);
    default:
      return false;
  }
}

base::Optional<std::string> LocalBinaryFinder::FindBinaryInRoot(
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

  std::string symbol_file = root_str + "/" + dirname + "/" + filename;
  if (access(symbol_file.c_str(), F_OK) == 0 &&
      IsCorrectFile(symbol_file, build_id))
    return {symbol_file};

  if (filename.find(kApkPrefix) == 0) {
    symbol_file =
        root_str + "/" + dirname + "/" + filename.substr(sizeof(kApkPrefix));
    if (access(symbol_file.c_str(), F_OK) == 0 &&
        IsCorrectFile(symbol_file, build_id))
      return {symbol_file};
  }

  symbol_file = root_str + "/" + filename;
  if (access(symbol_file.c_str(), F_OK) == 0 &&
      IsCorrectFile(symbol_file, build_id))
    return {symbol_file};

  if (filename.find(kApkPrefix) == 0) {
    symbol_file = root_str + "/" + filename.substr(sizeof(kApkPrefix));
    if (access(symbol_file.c_str(), F_OK) == 0 &&
        IsCorrectFile(symbol_file, build_id))
      return {symbol_file};
  }

  std::string hex_build_id = base::ToHex(build_id.c_str(), build_id.size());
  std::string split_hex_build_id = SplitBuildID(hex_build_id);
  if (!split_hex_build_id.empty()) {
    symbol_file =
        root_str + "/" + ".build-id" + "/" + split_hex_build_id + ".debug";
    if (access(symbol_file.c_str(), F_OK) == 0 &&
        IsCorrectFile(symbol_file, build_id))
      return {symbol_file};
  }

  return base::nullopt;
}

Subprocess::Subprocess(const std::string& file, std::vector<std::string> args)
    : input_pipe_(base::Pipe::Create(base::Pipe::kBothBlock)),
      output_pipe_(base::Pipe::Create(base::Pipe::kBothBlock)) {
  std::vector<char*> c_str_args(args.size() + 1, nullptr);
  for (std::string& arg : args)
    c_str_args.push_back(&(arg[0]));

  if ((pid_ = fork()) == 0) {
    // Child
    PERFETTO_CHECK(dup2(*input_pipe_.rd, STDIN_FILENO) != -1);
    PERFETTO_CHECK(dup2(*output_pipe_.wr, STDOUT_FILENO) != -1);
    input_pipe_.wr.reset();
    output_pipe_.rd.reset();
    if (execvp(file.c_str(), &(c_str_args[0])) == -1)
      PERFETTO_FATAL("Failed to exec %s", file.c_str());
  }
  PERFETTO_CHECK(pid_ != -1);
  input_pipe_.rd.reset();
  output_pipe_.wr.reset();
}

Subprocess::~Subprocess() {
  if (pid_ != -1) {
    kill(pid_, SIGKILL);
    int wstatus;
    PERFETTO_EINTR(waitpid(pid_, &wstatus, 0));
  }
}

LLVMSymbolizerProcess::LLVMSymbolizerProcess()
    : subprocess_("llvm-symbolizer", {"llvm-symbolizer"}),
      read_file_(fdopen(subprocess_.read_fd(), "r")) {}

std::vector<SymbolizedFrame> LLVMSymbolizerProcess::Symbolize(
    const std::string& binary,
    uint64_t address) {
  std::vector<SymbolizedFrame> result;

  if (PERFETTO_EINTR(dprintf(subprocess_.write_fd(), "%s 0x%" PRIx64 "\n",
                             binary.c_str(), address)) < 0) {
    PERFETTO_ELOG("Failed to write to llvm-symbolizer.");
    return result;
  }
  auto lines = GetLines(read_file_);
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
    const std::vector<uint64_t>& addresses) {
  base::Optional<std::string> binary =
      finder_.FindBinary(mapping_name, build_id);
  if (!binary)
    return {};
  std::vector<std::vector<SymbolizedFrame>> result;
  result.reserve(addresses.size());
  for (uint64_t address : addresses)
    result.emplace_back(llvm_symbolizer_.Symbolize(*binary, address));
  return result;
}

LocalSymbolizer::~LocalSymbolizer() = default;

}  // namespace profiling
}  // namespace perfetto

#endif  // PERFETTO_BUILDFLAG(PERFETTO_LOCAL_SYMBOLIZER)
