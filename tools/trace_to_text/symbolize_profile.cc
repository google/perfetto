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

#include "tools/trace_to_text/symbolize_profile.h"

#include <map>
#include <set>
#include <string>
#include <vector>

#include <elf.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "perfetto/protozero/proto_utils.h"

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/utils.h"

#include "tools/trace_to_text/utils.h"

#include "perfetto/trace/profiling/profile_common.pb.h"
#include "perfetto/trace/profiling/profile_packet.pb.h"
#include "perfetto/trace/trace.pbzero.h"
#include "perfetto/trace/trace_packet.pb.h"

#include "perfetto/trace/interned_data/interned_data.pb.h"

namespace perfetto {
namespace trace_to_text {
namespace {

using ::protozero::proto_utils::kMessageLengthFieldSize;
using ::protozero::proto_utils::MakeTagLengthDelimited;
using ::protozero::proto_utils::WriteVarInt;

using ::perfetto::protos::Frame;
using ::perfetto::protos::InternedData;
using ::perfetto::protos::InternedString;
using ::perfetto::protos::Mapping;
using ::perfetto::protos::ProfilePacket;

class Subprocess {
 public:
  Subprocess(const std::string file, std::vector<std::string> args)
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
      PERFETTO_CHECK(execvp(file.c_str(), &(c_str_args[0])) != -1);
    }
    PERFETTO_CHECK(pid_ != -1);
    input_pipe_.rd.reset();
    output_pipe_.wr.reset();
  }

  ~Subprocess() {
    if (pid_ != -1) {
      kill(pid_, SIGKILL);
      int wstatus;
      waitpid(pid_, &wstatus, 0);
    }
  }

  int read_fd() { return output_pipe_.rd.get(); }
  int write_fd() { return input_pipe_.wr.get(); }

 private:
  base::Pipe input_pipe_;
  base::Pipe output_pipe_;

  pid_t pid_ = -1;
};

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
};

struct Elf32 {
  using Ehdr = Elf32_Ehdr;
  using Shdr = Elf32_Shdr;
  using Nhdr = Elf32_Nhdr;
};

struct Elf64 {
  using Ehdr = Elf64_Ehdr;
  using Shdr = Elf64_Shdr;
  using Nhdr = Elf64_Nhdr;
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

class LocalBinaryFinder {
 public:
  LocalBinaryFinder(std::vector<std::string> roots)
      : roots_(std::move(roots)) {}

  base::Optional<std::string> FindBinary(const std::string& abspath,
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
    PERFETTO_ELOG("Could not find %s.", abspath.c_str());
    return cache_entry;
  }

 private:
  bool IsCorrectFile(const std::string& symbol_file,
                     const std::string& build_id) {
    base::ScopedFile fd(base::OpenFile(symbol_file, O_RDONLY));
    if (!fd)
      return false;

    struct stat statbuf;
    if (fstat(*fd, &statbuf) == -1)
      return false;

    size_t size = static_cast<size_t>(statbuf.st_size);

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

  base::Optional<std::string> FindBinaryInRoot(const std::string& root_str,
                                               const std::string& abspath,
                                               const std::string& build_id) {
    constexpr char kApkPrefix[] = "base.apk!";

    std::string filename;
    std::string dirname;

    for (base::StringSplitter sp(abspath, '/'); sp.Next();) {
      dirname += "/" + filename;
      filename = sp.cur_token();
    }

    // Return the first match for the following options:
    // * absolute path of library file relative to root.
    // * absolute path of library file relative to root, but with base.apk!
    //   removed from filename.
    // * only filename of library file relative to root.
    // * only filename of library file relative to root, but with base.apk!
    //   removed from filename.
    //
    // For example, "/system/lib/base.apk!foo.so" is looked for at
    // * $ROOT/system/lib/base.apk!foo.so
    // * $ROOT/system/lib/foo.so
    // * $ROOT/base.apk!foo.so
    // * $ROOT/foo.so

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

    return base::nullopt;
  }

 private:
  std::vector<std::string> roots_;
  std::map<std::string, base::Optional<std::string>> cache_;
};

struct SymbolizedFrame {
  std::string function_name;
  std::string line_information;
};

class LocalLLVMSymbolizer {
 public:
  LocalLLVMSymbolizer()
      : subprocess_("llvm-symbolizer", {"llvm-symbolizer"}),
        read_file_(fdopen(subprocess_.read_fd(), "r")) {}

  std::vector<SymbolizedFrame> Symbolize(const std::string& binary,
                                         uint64_t address) {
    std::vector<SymbolizedFrame> result;

    if (PERFETTO_EINTR(dprintf(subprocess_.write_fd(), "%s 0x%lx\n",
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
      if (i % 2 == 0)
        result[i / 2].function_name = lines[i];
      else
        result[i / 2].line_information = lines[i];
    }
    return result;
  }

 private:
  Subprocess subprocess_;
  FILE* read_file_;
};

std::vector<std::string> GetRootsForEnv() {
  std::vector<std::string> roots;
  const char* root = getenv("PERFETTO_BINARY_PATH");
  if (root != nullptr) {
    for (base::StringSplitter sp(std::string(root), ':'); sp.Next();)
      roots.emplace_back(sp.cur_token(), sp.cur_token_size());
  }
  return roots;
}

class Symbolizer {
 public:
  Symbolizer() : finder_(GetRootsForEnv()) {}

  void AddInternedString(const InternedString& string) {
    interned_strings_.emplace(string.iid(), string.str());
  }

  void AddMapping(const Mapping& mapping) {
    mappings_.emplace(mapping.iid(), ResolveMapping(mapping));
  }

  void SymbolizeFrame(Frame* frame) {
    auto it = mappings_.find(frame->mapping_id());
    if (it == mappings_.end()) {
      PERFETTO_ELOG("Invalid mapping.");
      return;
    }
    const ResolvedMapping& mapping = it->second;
    base::Optional<std::string> binary =
        finder_.FindBinary(mapping.mapping_name, mapping.build_id);
    if (!binary)
      return;
    auto result = llvm_symbolizer_.Symbolize(*binary, frame->rel_pc());
    if (!result.empty()) {
      // TODO(fmayer): Better support for inline functions.
      const SymbolizedFrame& symf = result[0];
      if (symf.function_name != "??") {
        uint64_t& id = intern_table_[symf.function_name];
        if (!id)
          id = --intern_id_;
        frame->set_function_name_id(id);
      }
    }
  }

  const std::map<std::string, uint64_t>& intern_table() const {
    return intern_table_;
  }

 private:
  struct ResolvedMapping {
    std::string mapping_name;
    std::string build_id;
  };

  std::string ResolveString(uint64_t iid) {
    auto it = interned_strings_.find(iid);
    if (it == interned_strings_.end())
      return {};
    return it->second;
  }

  ResolvedMapping ResolveMapping(const Mapping& mapping) {
    std::string path;
    for (uint64_t iid : mapping.path_string_ids()) {
      path += "/";
      path += ResolveString(iid);
    }
    return {std::move(path), ResolveString(mapping.build_id())};
  }

  LocalLLVMSymbolizer llvm_symbolizer_;
  LocalBinaryFinder finder_;

  std::map<uint64_t, std::string> interned_strings_;
  std::map<uint64_t, ResolvedMapping> mappings_;

  std::map<std::string, uint64_t> intern_table_;
  // Use high IDs for the newly interned strings to avoid clashing with
  // other interned strings. The other solution is to read the trace twice
  // in order to find out the maximum used interned ID. This means that we
  // cannot operate on stdin anymore.
  uint64_t intern_id_ = std::numeric_limits<uint64_t>::max();
};

void WriteTracePacket(const std::string str, std::ostream* output) {
  constexpr char kPreamble =
      MakeTagLengthDelimited(protos::pbzero::Trace::kPacketFieldNumber);
  uint8_t length_field[10];
  uint8_t* end = WriteVarInt(str.size(), length_field);
  *output << kPreamble;
  *output << std::string(length_field, end);
  *output << str;
}

}  // namespace

int SymbolizeProfile(std::istream* input, std::ostream* output) {
  Symbolizer symbolizer;

  ForEachPacketInTrace(input, [&output,
                               &symbolizer](protos::TracePacket packet) {
    protos::ProfilePacket* profile_packet = nullptr;
    if (packet.has_profile_packet()) {
      profile_packet = packet.mutable_profile_packet();
    }
    InternedData* data = nullptr;
    if (packet.has_interned_data())
      data = packet.mutable_interned_data();
    if (profile_packet) {
      for (const InternedString& interned_string : profile_packet->strings())
        symbolizer.AddInternedString(interned_string);
    }
    if (data) {
      for (const InternedString& interned_string : data->build_ids())
        symbolizer.AddInternedString(interned_string);
      for (const InternedString& interned_string : data->mapping_paths())
        symbolizer.AddInternedString(interned_string);
      for (const InternedString& interned_string : data->function_names())
        symbolizer.AddInternedString(interned_string);
    }
    if (profile_packet) {
      for (const Mapping& mapping : profile_packet->mappings())
        symbolizer.AddMapping(mapping);
    }
    if (data) {
      for (const Mapping& mapping : data->mappings())
        symbolizer.AddMapping(mapping);
    }
    if (profile_packet) {
      for (Frame& frame : *profile_packet->mutable_frames())
        symbolizer.SymbolizeFrame(&frame);
    }
    if (data) {
      for (Frame& frame : *data->mutable_frames())
        symbolizer.SymbolizeFrame(&frame);
    }

    // As we will write the newly interned strings after, we need to set
    // continued for the last ProfilePacket.
    if (profile_packet)
      profile_packet->set_continued(true);
    WriteTracePacket(packet.SerializeAsString(), output);
  });

  // We have to emit a ProfilePacket with continued = false to terminate the
  // sequence of related ProfilePackets.
  protos::TracePacket packet;
  const auto& intern_table = symbolizer.intern_table();
  if (!intern_table.empty()) {
    InternedData* data = packet.mutable_interned_data();
    for (const auto& p : intern_table) {
      InternedString* str = data->add_function_names();
      str->set_iid(p.second);
      str->set_str(p.first);
    }
  }
  packet.mutable_profile_packet();
  WriteTracePacket(packet.SerializeAsString(), output);
  return 0;
}

}  // namespace trace_to_text
}  // namespace perfetto
