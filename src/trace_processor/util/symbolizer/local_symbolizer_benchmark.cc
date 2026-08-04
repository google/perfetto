// Copyright (C) 2026 The Android Open Source Project
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

// Benchmarks for the LocalBinaryIndexer directory walk (BuildIdIndex).
//
//   * BM_SymbolIndexerAndroidSymbols generates a tree shaped like an Android
//     symbols/ directory (partition/bin/lib/lib64 hierarchy, ELF-heavy with
//     non-ELF files, soname symlinks and symlinks pointing OUTSIDE the
//     indexed tree, which is intended, supported behaviour). Files larger
//     than 64 KiB are written sparsely so the logical size distribution is
//     realistic without materializing tens of gigabytes on disk.
//   * BM_SymbolIndexerBundleDefaults indexes the paths `bundle` would
//     auto-discover on this machine (e.g. /usr/lib/debug, ~/.debug,
//     ANDROID_PRODUCT_OUT/symbols, PERFETTO_BINARY_PATH).
//   * BM_SymbolIndexerRealTree indexes an actual directory supplied via the
//     PERFETTO_SYMBOL_TREE environment variable (no generation).
//
// Trees are generated once per benchmark/argument combination (in a temp dir
// kept alive for the process) and then re-indexed repeatedly, so the timing
// reflects steady-state indexing with a warm page cache, which is the typical
// case for repeated `trace_processor bundle` / traceconv runs against the
// same --symbol-paths directories.

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "src/trace_processor/util/symbolizer/elf.h"
#include "src/trace_processor/util/symbolizer/local_symbolizer.h"

#if PERFETTO_BUILDFLAG(PERFETTO_LOCAL_SYMBOLIZER)

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
#include <unistd.h>
#endif

namespace perfetto {
namespace profiling {
namespace {

constexpr size_t kBuildIdLen = 20;

bool IsFunctionalOnly() {
  return getenv("BENCHMARK_FUNCTIONAL_TEST_ONLY") != nullptr;
}

std::string BuildIdFor(size_t idx) {
  std::string out(kBuildIdLen, '\0');
  for (size_t i = 0; i < sizeof(idx); i++) {
    out[i] = static_cast<char>((idx >> (8 * i)) & 0xff);
  }
  return out;
}

void WriteFile(const std::string& path, const std::string& content) {
  base::ScopedFile fd(base::OpenFile(path, O_CREAT | O_WRONLY | O_TRUNC, 0600));
  PERFETTO_CHECK(fd);
  PERFETTO_CHECK(base::WriteAll(*fd, content.data(), content.size()) ==
                 static_cast<ssize_t>(content.size()));
}

// A small (a few hundred bytes) valid ELF64 with a build-id note, same layout
// as the fixture used by local_symbolizer_unittest.cc.
std::string CreateSmallElf(const std::string& build_id) {
  struct SimpleElf {
    Elf64::Ehdr ehdr;
    Elf64::Shdr shdr;
    Elf64::Phdr phdr;
    Elf64::Nhdr nhdr;
    char note_name[4];
    char note_desc[20];
  } e;
  memset(&e, 0, sizeof e);

  e.ehdr.e_ident[EI_MAG0] = ELFMAG0;
  e.ehdr.e_ident[EI_MAG1] = ELFMAG1;
  e.ehdr.e_ident[EI_MAG2] = ELFMAG2;
  e.ehdr.e_ident[EI_MAG3] = ELFMAG3;
  e.ehdr.e_ident[EI_CLASS] = ELFCLASS64;
  e.ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
  e.ehdr.e_ident[EI_VERSION] = EV_CURRENT;
  e.ehdr.e_version = EV_CURRENT;
  e.ehdr.e_shentsize = sizeof(Elf64::Shdr);
  e.ehdr.e_shnum = 1;
  e.ehdr.e_ehsize = sizeof e.ehdr;
  e.ehdr.e_shoff = offsetof(SimpleElf, shdr);
  e.ehdr.e_phnum = 2;
  e.ehdr.e_phoff = offsetof(SimpleElf, phdr);
  e.ehdr.e_phentsize = sizeof(Elf64::Phdr);

  e.shdr.sh_type = SHT_NOTE;
  e.shdr.sh_offset = offsetof(SimpleElf, nhdr);

  e.phdr.p_type = PT_LOAD;
  e.phdr.p_flags = PF_X;

  e.nhdr.n_type = NT_GNU_BUILD_ID;
  e.nhdr.n_namesz = sizeof e.note_name;
  e.nhdr.n_descsz = sizeof e.note_desc;
  strcpy(e.note_name, "GNU");
  memcpy(e.note_desc, build_id.c_str(),
         std::min(build_id.size(), sizeof(e.note_desc)));

  e.shdr.sh_size = offsetof(SimpleElf, note_desc) + sizeof(e.note_desc) -
                   offsetof(SimpleElf, nhdr);

  return std::string(reinterpret_cast<const char*>(&e), sizeof e);
}

// Writes an ELF64 of `file_size` bytes whose section header table (holding a
// build-id note) sits at the *end* of the file, mimicking a large unstripped
// binary where parsing requires faulting in pages far from the ELF header.
// The padding is skipped with a seek (keeping the file sparse), so the
// logical size is realistic without materializing it on disk.
void WriteLargeElf(const std::string& path,
                   size_t file_size,
                   const std::string& build_id) {
  constexpr size_t kShdrCount = 64;
  const size_t shdr_table = kShdrCount * sizeof(Elf64::Shdr);
  const size_t note_len = sizeof(Elf64::Nhdr) + 4 + kBuildIdLen;
  PERFETTO_CHECK(file_size > sizeof(Elf64::Ehdr) + sizeof(Elf64::Phdr) +
                                 shdr_table + note_len);
  const size_t shoff = file_size - shdr_table - note_len;

  base::ScopedFile fd(base::OpenFile(path, O_CREAT | O_WRONLY | O_TRUNC, 0600));
  PERFETTO_CHECK(fd);

  Elf64::Ehdr ehdr;
  memset(&ehdr, 0, sizeof(ehdr));
  ehdr.e_ident[EI_MAG0] = ELFMAG0;
  ehdr.e_ident[EI_MAG1] = ELFMAG1;
  ehdr.e_ident[EI_MAG2] = ELFMAG2;
  ehdr.e_ident[EI_MAG3] = ELFMAG3;
  ehdr.e_ident[EI_CLASS] = ELFCLASS64;
  ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
  ehdr.e_ident[EI_VERSION] = EV_CURRENT;
  ehdr.e_version = EV_CURRENT;
  ehdr.e_ehsize = sizeof(ehdr);
  ehdr.e_phoff = sizeof(ehdr);
  ehdr.e_phentsize = sizeof(Elf64::Phdr);
  ehdr.e_phnum = 1;
  ehdr.e_shentsize = sizeof(Elf64::Shdr);
  ehdr.e_shnum = static_cast<Elf64::Half>(kShdrCount);
  ehdr.e_shoff = shoff;
  PERFETTO_CHECK(base::WriteAll(*fd, &ehdr, sizeof(ehdr)) ==
                 static_cast<ssize_t>(sizeof(ehdr)));

  Elf64::Phdr phdr;
  memset(&phdr, 0, sizeof(phdr));
  phdr.p_type = PT_LOAD;
  phdr.p_flags = PF_X;
  PERFETTO_CHECK(base::WriteAll(*fd, &phdr, sizeof(phdr)) ==
                 static_cast<ssize_t>(sizeof(phdr)));

  // Padding up to the section header table (seek past it, keeping the file
  // sparse; zero-fill on Windows where lseek on the fd is not available).
  const size_t padded = shoff - sizeof(ehdr) - sizeof(phdr);
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  PERFETTO_CHECK(lseek(fd.get(), static_cast<off_t>(padded), SEEK_CUR) !=
                 static_cast<off_t>(-1));
#else
  {
    std::string zeros(1 << 20, '\0');
    size_t to_write = padded;
    while (to_write > 0) {
      size_t chunk = std::min(zeros.size(), to_write);
      PERFETTO_CHECK(base::WriteAll(*fd, zeros.data(), chunk) ==
                     static_cast<ssize_t>(chunk));
      to_write -= chunk;
    }
  }
#endif

  // Section header table: shdr[0] points at the build-id note.
  std::vector<Elf64::Shdr> shdrs(kShdrCount);
  memset(shdrs.data(), 0, shdrs.size() * sizeof(Elf64::Shdr));
  shdrs[0].sh_type = SHT_NOTE;
  shdrs[0].sh_offset = shoff + shdr_table;
  shdrs[0].sh_size = note_len;
  PERFETTO_CHECK(
      base::WriteAll(*fd, shdrs.data(), shdrs.size() * sizeof(Elf64::Shdr)) ==
      static_cast<ssize_t>(shdrs.size() * sizeof(Elf64::Shdr)));

  // The note itself: Nhdr + "GNU" + build-id.
  Elf64::Nhdr nhdr;
  memset(&nhdr, 0, sizeof(nhdr));
  nhdr.n_type = NT_GNU_BUILD_ID;
  nhdr.n_namesz = 4;
  nhdr.n_descsz = kBuildIdLen;
  PERFETTO_CHECK(base::WriteAll(*fd, &nhdr, sizeof(nhdr)) ==
                 static_cast<ssize_t>(sizeof(nhdr)));
  PERFETTO_CHECK(base::WriteAll(*fd, "GNU", 4) == 4);
  PERFETTO_CHECK(base::WriteAll(*fd, build_id.data(), kBuildIdLen) ==
                 static_cast<ssize_t>(kBuildIdLen));
}

// Deterministic non-ELF content (never starts with the ELF magic).
std::string CreateJunk(size_t size) {
  std::string out(size, '\0');
  for (size_t i = 0; i < size; i++) {
    out[i] = static_cast<char>('a' + (i % 26));
  }
  return out;
}

bool MkdirRecursive(const std::string& path) {
  std::string cur;
  for (char c : path) {
    cur += c;
    if (c == '/') {
      if (!base::Mkdir(cur) && errno != EEXIST)
        return false;
    }
  }
  if (!base::Mkdir(cur) && errno != EEXIST)
    return false;
  return true;
}

size_t LogUniform(std::mt19937& rng, size_t lo, size_t hi) {
  const double u =
      std::generate_canonical<double, std::numeric_limits<double>::digits>(rng);
  const double l = std::log(static_cast<double>(lo));
  const double h = std::log(static_cast<double>(hi));
  return static_cast<size_t>(std::exp(l + u * (h - l)));
}

// Lazily builds a tree once per key and keeps it alive for the process. The
// container is intentionally leaked to avoid an exit-time destructor.
const std::string& GetTree(const std::string& key,
                           std::function<void(const std::string&)> builder) {
  static auto* trees =
      new std::map<std::string, std::unique_ptr<base::TempDir>>();
  auto it = trees->find(key);
  if (it == trees->end()) {
    std::unique_ptr<base::TempDir> dir(
        new base::TempDir(base::TempDir::Create()));
    builder(dir->path());
    it = trees->emplace(key, std::move(dir)).first;
  }
  return it->second->path();
}

// --- Android symbols/ tree generator ----------------------------------------

enum class DirKind { kBin, kLib, kJunkOnly };

struct DirSpec {
  const char* rel;  // path under <root>/symbols
  double weight;    // share of the total file count
  DirKind kind;
  const char* junk_ext;  // extension for non-ELF files in this dir
};

// Roughly the shape of an Android symbols/ tree (partitions x bin/lib/lib64,
// lib64 dominating, plus non-ELF dirs).
constexpr DirSpec kAndroidDirs[] = {
    {"system/bin", 8, DirKind::kBin, ".sh"},
    {"system/lib", 6, DirKind::kLib, ".txt"},
    {"system/lib64", 16, DirKind::kLib, ".txt"},
    {"system/etc", 2, DirKind::kJunkOnly, ".xml"},
    {"system/framework", 3, DirKind::kJunkOnly, ".jar"},
    {"system/app", 2, DirKind::kJunkOnly, ".apk"},
    {"system/priv-app", 2, DirKind::kJunkOnly, ".apk"},
    {"vendor/bin", 5, DirKind::kBin, ".sh"},
    {"vendor/lib", 4, DirKind::kLib, ".txt"},
    {"vendor/lib64", 12, DirKind::kLib, ".txt"},
    {"vendor/etc", 1, DirKind::kJunkOnly, ".xml"},
    {"product/bin", 2, DirKind::kBin, ".sh"},
    {"product/lib64", 5, DirKind::kLib, ".txt"},
    {"apex/com.android.art/lib64", 4, DirKind::kLib, ".txt"},
    {"apex/com.android.runtime/lib64", 4, DirKind::kLib, ".txt"},
    {"apex/com.android.vndk/lib64", 3, DirKind::kLib, ".txt"},
    {"recovery/root/system/bin", 1, DirKind::kBin, ".sh"},
    {"recovery/root/system/lib64", 2, DirKind::kLib, ".txt"},
};

constexpr double kAndroidDirsWeight = []() {
  double w = 0;
  for (const auto& d : kAndroidDirs)
    w += d.weight;
  return w;
}();

void BuildAndroidSymbolsTree(const std::string& root, size_t num_files) {
  std::mt19937 rng(static_cast<uint32_t>(0x12345678u ^ num_files));

  size_t file_idx = 0;
  for (const auto& spec : kAndroidDirs) {
    const size_t count = static_cast<size_t>(static_cast<double>(num_files) *
                                             spec.weight / kAndroidDirsWeight);
    const std::string dir = root + "/symbols/" + spec.rel;
    PERFETTO_CHECK(MkdirRecursive(dir));

    for (size_t k = 0; k < count; k++, file_idx++) {
      // Force the very first file to be an ELF so the benchmark can assert on
      // a known build-id; everything else follows the ~85% ELF mix.
      const bool is_elf = spec.kind != DirKind::kJunkOnly &&
                          (file_idx == 0 || rng() % 100 < 85);
      if (is_elf) {
        const size_t size = LogUniform(rng, 16 << 10, 8 << 20);
        const std::string name =
            spec.kind == DirKind::kLib
                ? "libmod" + std::to_string(file_idx) + ".so"
                : "prog" + std::to_string(file_idx);
        if (size <= 64 << 10) {
          WriteFile(dir + "/" + name, CreateSmallElf(BuildIdFor(file_idx)));
        } else {
          // Sparse write with a modest section count: keeps the disk
          // footprint of the generated tree small (a real tree can be
          // measured directly via BM_SymbolIndexerRealTree instead).
          WriteLargeElf(dir + "/" + name, size, BuildIdFor(file_idx));
        }
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
        // Soname-style alias (libmodX.so.1 -> libmodX.so), like real trees.
        if (spec.kind == DirKind::kLib && rng() % 100 < 25) {
          PERFETTO_CHECK(
              symlink(name.c_str(), (dir + "/" + name + ".1").c_str()) == 0);
        }
#endif
      } else {
        const char* prefix =
            spec.kind == DirKind::kLib
                ? "libjunk"
                : (spec.kind == DirKind::kBin ? "script" : "junk");
        WriteFile(dir + "/" + prefix + std::to_string(file_idx) + spec.junk_ext,
                  CreateJunk(256 + rng() % (4 << 10)));
      }
    }
  }

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  // A few files living OUTSIDE the indexed tree, reachable via symlinks from
  // inside it: following symlinks out of the tree is intended, supported
  // behaviour.
  PERFETTO_CHECK(base::Mkdir(root + "/extras"));
  for (size_t i = 0; i < 8; i++) {
    WriteFile(root + "/extras/xtra" + std::to_string(i),
              CreateSmallElf(BuildIdFor(1000000 + i)));
    PERFETTO_CHECK(symlink(("../../../extras/xtra" + std::to_string(i)).c_str(),
                           (root + "/symbols/vendor/lib64/xtra_link" +
                            std::to_string(i) + ".so")
                               .c_str()) == 0);
  }
#endif
}

// --- Scenarios --------------------------------------------------------------

static void BM_SymbolIndexerAndroidSymbols(benchmark::State& state) {
  const size_t num_files = static_cast<size_t>(state.range(0));
  std::string dir = GetTree("android" + std::to_string(num_files),
                            [&](const std::string& root) {
                              BuildAndroidSymbolsTree(root, num_files);
                            });
  for (auto _ : state) {
    LocalBinaryIndexer indexer({dir + "/symbols"}, {});
    // file_idx 0 is always an ELF (see BuildAndroidSymbolsTree).
    PERFETTO_CHECK(indexer.FindBinary("", BuildIdFor(0)).ok());
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
    // The out-of-tree symlink targets must be reachable.
    PERFETTO_CHECK(indexer.FindBinary("", BuildIdFor(1000000)).ok());
#endif
  }
}

static void BM_SymbolIndexerBundleDefaults(benchmark::State& state) {
  // Mirror bundle's symbol path resolution (trace_to_bundle.cc + the
  // DiscoverSymbolPaths logic in trace_enrichment.cc): PERFETTO_BINARY_PATH,
  // then well-known locations with root_dir="/" and home_dir=$HOME, exactly
  // like the bundle subcommand computes them.
  std::vector<std::string> dirs;
  if (const char* env = getenv("PERFETTO_BINARY_PATH"); env && *env) {
    dirs = base::SplitString(env, ":");
  }
  const char* android_out = getenv("ANDROID_PRODUCT_OUT");
  const char* home = getenv("HOME");
  if (android_out && *android_out)
    dirs.push_back(std::string(android_out) + "/symbols");
  dirs.push_back("/usr/lib/debug");
  if (home && *home)
    dirs.push_back(std::string(home) + "/.debug");

  std::vector<std::string> existing;
  for (const auto& d : dirs) {
    if (base::FileExists(d))
      existing.push_back(d);
  }
  if (existing.empty()) {
    state.SkipWithError(
        "no bundle symbol paths found on this machine (set PERFETTO_BINARY_PATH"
        " or install debug symbols)");
    return;
  }
  for (auto _ : state) {
    LocalBinaryIndexer indexer(existing, {});
  }
}

static void BM_SymbolIndexerRealTree(benchmark::State& state) {
  const char* tree = getenv("PERFETTO_SYMBOL_TREE");
  if (!tree || !*tree) {
    state.SkipWithError(
        "set PERFETTO_SYMBOL_TREE to a symbol directory to enable");
    return;
  }
  std::string dir = tree;
  for (auto _ : state) {
    LocalBinaryIndexer indexer({dir}, {});
  }
}

// --- Registration -----------------------------------------------------------

static void AndroidSymbolsArgs(benchmark::internal::Benchmark* b) {
  if (IsFunctionalOnly()) {
    b->Range(512, 512);
  } else {
    b->RangeMultiplier(2)->Range(4096, 32768);
  }
}
BENCHMARK(BM_SymbolIndexerAndroidSymbols)->Apply(AndroidSymbolsArgs);

// No args: indexes whatever PERFETTO_SYMBOL_TREE points at.
BENCHMARK(BM_SymbolIndexerRealTree);

// No args: indexes the symbol paths bundle would auto-discover on this
// machine.
BENCHMARK(BM_SymbolIndexerBundleDefaults);

}  // namespace
}  // namespace profiling
}  // namespace perfetto

#endif  // PERFETTO_BUILDFLAG(PERFETTO_LOCAL_SYMBOLIZER)
