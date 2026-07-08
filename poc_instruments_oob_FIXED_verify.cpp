// Verification harness: same scenario as poc_instruments_oob.cpp
// (<thread ref="999"/> with no prior <thread id="999">), but using the FIXED
// logic (RowDataTracker::At() bounds-check + caller null-check), to confirm
// the out-of-bounds access / crash is actually resolved by the patch on
// branch patch/instruments-xml-oob-read.
//
// Build & run (release-equivalent, NDEBUG):
//   g++ -DNDEBUG -O0 -std=c++17 -g -o poc_fixed.exe poc_instruments_oob_FIXED_verify.cpp
//   ./poc_fixed.exe

#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>

#ifdef NDEBUG
#define PERFETTO_DCHECK(x) do {} while (0)
#else
#define PERFETTO_DCHECK(x) do { if (!(x)) { std::abort(); } } while (0)
#endif

using ThreadId = uint32_t;
constexpr uint32_t kNullId = 0u;

struct Thread {
  int tid = 0;
  int fmt = 0;
  ThreadId process = kNullId;
};

class RowDataTracker {
 public:
  struct IdPtr { uint32_t id; Thread* ptr; };

  IdPtr NewThread() {
    ThreadId id = static_cast<ThreadId>(threads_.size());
    threads_.push_back(Thread());
    return {id + 1, &threads_.back()};
  }

  // FIXED: real bounds check, returns nullptr instead of indexing OOB.
  Thread* GetThread(ThreadId id) {
    if (id == kNullId || id > threads_.size()) {
      return nullptr;
    }
    return &threads_[id - 1];
  }

  size_t ThreadCount() const { return threads_.size(); }

 private:
  std::vector<Thread> threads_;
};

template <typename Value>
struct MaybeCachedRef { Value& ref; bool is_new; };

template <typename Value>
MaybeCachedRef<Value> GetOrInsertByRef(const char** attrs, std::map<unsigned long, Value>& map) {
  static constexpr unsigned long kInvalidRefId = ~0UL;
  if (attrs[0] == nullptr || attrs[1] == nullptr ||
      (strcmp(attrs[0], "ref") != 0 && strcmp(attrs[0], "id") != 0)) {
    return {map[kInvalidRefId], false};
  }
  unsigned long id = strtoul(attrs[1], nullptr, 10);
  bool is_new = strcmp(attrs[0], "id") == 0;
  return {map[id], is_new};
}

int main() {
  RowDataTracker data;
  std::map<unsigned long, ThreadId> thread_ref_to_thread;

  std::cout << "=== Case 1: <thread ref=\"999\"/> with no prior id=\"999\" ===\n";
  const char* attrs1[] = {"ref", "999", nullptr};
  auto thread_lookup = GetOrInsertByRef(attrs1, thread_ref_to_thread);
  ThreadId row_thread = thread_lookup.ref;
  std::cout << "resolved id = " << row_thread << "\n";

  // Fixed RowParser::Parse()-equivalent logic:
  Thread* thread = data.GetThread(row_thread);
  if (!thread) {
    std::cout << "GetThread() returned nullptr -> row safely skipped "
                 "(no crash, no OOB access).\n";
  } else {
    std::cout << "thread->tid = " << thread->tid << "\n";
  }

  std::cout << "\n=== Case 2: <thread/> with no attributes at all ===\n";
  const char* attrs2[] = {nullptr};
  auto thread_lookup2 = GetOrInsertByRef(attrs2, thread_ref_to_thread);
  std::cout << "resolved id = " << thread_lookup2.ref
            << " (no crash inside GetOrInsertByRef)\n";

  std::cout << "\n=== Case 3: legitimate <thread id=\"1\"/> then <thread ref=\"1\"/> ===\n";
  const char* attrs3[] = {"id", "1", nullptr};
  auto new_thread_lookup = GetOrInsertByRef(attrs3, thread_ref_to_thread);
  if (new_thread_lookup.is_new) {
    auto new_thread = data.NewThread();
    new_thread_lookup.ref = new_thread.id;
    new_thread.ptr->tid = 42;
  }
  const char* attrs4[] = {"ref", "1", nullptr};
  auto ref_lookup = GetOrInsertByRef(attrs4, thread_ref_to_thread);
  Thread* legit_thread = data.GetThread(ref_lookup.ref);
  std::cout << "legit_thread " << (legit_thread ? "resolved OK, tid=" : "NULL")
            << (legit_thread ? std::to_string(legit_thread->tid) : "") << "\n";

  std::cout << "\nALL CASES COMPLETED WITHOUT A CRASH.\n";
  return 0;
}
