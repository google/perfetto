# LockFreeTaskRunner Design Document

## Overview

[`base::LockFreeTaskRunner`](/include/perfetto/ext/base/lock_free_task_runner.h)
is a cross-platform lock-free multi-producer single-consumer task execution
engine that underpins most of Perfetto's code, both SDK and on-device services.

It provides thread-safe task posting from multiple threads while ensuring all
task execution happens on a single designated thread, eliminating the need for
traditional mutex-based synchronization.

Key properties:

* No mutexes or spinlocks: bounded time for PostTask() and Run().
* In the absence of bursts (i.e. no more than 2 x 512 = 1024 tasks outstanding) no
  allocations are performed, other than the ones that might be required to copy
  around non-trivial `std::function<void()>`s.
* Compatible behaviour with the legacy UnixTaskRunner: tasks are extracted and
  processed in the same order.

On top of avoiding lock contention, this new task runner is ~2x faster than our
legacy UnixTaskRunner:

```
$ out/rel/perfetto_benchmarks --benchmark_filter='.*BM_TaskRunner.*'
...
-------------------------------------------------------------------------------------------
Benchmark                                                 Time             CPU   Iterations
-------------------------------------------------------------------------------------------
BM_TaskRunner_SingleThreaded<UnixTaskRunner>       27778190 ns     27772029 ns           25
BM_TaskRunner_SingleThreaded<LockFreeTaskRunner>   10381056 ns     10375656 ns           67
BM_TaskRunner_MultiThreaded<UnixTaskRunner>          567794 ns       344625 ns         2033
BM_TaskRunner_MultiThreaded<LockFreeTaskRunner>      265943 ns       265754 ns         2749
```

## Architecture

In the rest of this document we are going to refer to threads as:

* **Writers** the N threads that invoke `PostTask()`.
* **Reader** the one thread that runs tasks in the `Run()` loop.

This document focuses only on the design of PostTask() and does not discuss
PostDelayedTask() or Add/RemoveFileDescriptorWatch() at all. The logic of these
functions is unchanged from the legacy UnixTaskRunner and they are simply
implemented by hopping first to the task runner thread, and manipulating the
delayed task list and FD set on the main thread.
This involves an extra hop (if called from other threads) vs the legacy
UnixTaskRunner. However: (1) in practice most calls to PostDelayedTask() and
Add/RemoveFileDescriptorWatch() happen on the main thread in our codebase; (2)
they are almost never hot paths.


### Slab-Based Architecture

The LockFreeTaskRunner implements a **Multi-Producer Single-Consumer (MPSC)**
queue using a slab-based approach.

A Slab contains:

- **Task array**: Fixed-size array of 512 task slots (`kSlabSize`).
  These are written by the writer threads and consumed by the reader thread.

- **Slot counter**: Atomic counter `next_task_slot` for reserving slots.
  This is used to identify which slot in the array a writer should take (or to
  figure out that the slab is full).
  This is only accessed by the writer threads, never by the reader.
  When the slab is full this can grow > `kSlabSize` in case of races (it's fine,
  once all slots are full, the value of `next_task_slot` becomes useless).

- **Publication bitmap**: `tasks_written`. A fixed-size bitmap of 512 bits, one
  per task. Bits are flipped with an atomic release-OR operation by the writer
  thread to indicate that a task in the i-th slot is ready and can be consumed.
  The reader thread never alters this bitmap. Eventually this becomes 0xff..ff
  and stays like that for all the lifetime of the Slab.

- **Consumption bitmap**: `tasks_read`. Similar to the above, but this is only
  accessed by the reader thread. Bits are flipped to 1 as tasks are consumed.
  The writer thread never accesses this. Eventually this also becomes 0xff..ff.
  A Slab can be deleted only when both bitmaps are filled (all task slots have
  been written by the writers and consumed by the reader).

- **Linked list pointer**: `prev` pointing to the previous Slab.
  This is traversed only by the reader thread. The writers only look at the
  latest slab and never access the prev pointer (other than when constructing a
  new Slab)

Slabs are arranged as a singly-linked list.

Note that this list is NOT atomic, only the `tail_` pointer is. The reader
thread is the only one traversing the list, the writers only access the latest
Slab, and eventually append new Slabs, replacing the `tail_`.


```
           tail_ (atomic_shared_ptr)
                    |
                    ▼
  +-----------------+      +-----------------+      +-----------------+
  |     Slab N      |      |    Slab N-1     |      |     Slab 0      |
  | tasks: [....]   |      | tasks: [....]   |      | tasks: [....]   |
  | next_task_slot  |      | next_task_slot  |      | next_task_slot  |
  | prev (sptr) ----+----->| prev (sptr) ----+----->| prev = nullptr  |
  +-----------------+      +-----------------+      +-----------------+
```

1. **Unidirectional Access**: Producer threads only access the `tail` slab and
   never walk backwards.
2. **Consumer Ownership**: Only the main thread follows `prev` pointers and
   drains tasks.
3. **Burst Handling**: New slabs are allocated automatically by writers when the current
   slab fills.

In nominal conditions (i.e. in the absence of bursts of thousands of tasks) we will
have only two slabs. A freelist of size 1 (`free_slab_`) avoids pressure on the
allocator, effectively flipping between the two slabs without new/delete.

A singly-linked list with only a tail pointer suggests that the reader has a
worst case complexity of O(N), as it has to traverse the whole list to get to
the first tasks (it must run tasks FIFO). However, in practice we expect to have
only two slabs ever (and if we have a queue of 10k-100k tasks, walking the list
is our last problem).

The main compromise of this design is that it scales poorly with a large number
of tasks, as Run() becomes both slower (to traverse the list) and stack-greedy
(it uses recursion on the stack to walk the list without using the heap).
We don't expect a large number of outstanding tasks in Perfetto (with the
exception of known issues like b/330580374 which should be fixed regardless).

## Threading Considerations

### Producer Thread Workflow

The `PostTask()` operation follows this lock-free protocol:

1. **Load Tail**: Atomically load the current `tail_` slab pointer
2. **Acquire Refcount**: Increment refcount bucket for this Slab (discussed later)
3. **Reserve Slot**: Atomically increment `next_task_slot` to reserve a position
4. **Handle Overflow**: If slab is full, allocate new slab and try to atomically update `tail_`
5. **Write Task**: Store the task in the reserved slot
6. **Publish**: Set corresponding bit in `tasks_written` bitmask with release semantics
7. **Release Refcount**: Automatically decremented when `ScopedRefcount` destructor runs

#### Overflow Handling

When a slab becomes full (`slot >= kSlabSize`):

```cpp
Slab* new_slab = AllocNewSlab();
new_slab->prev = slab;
new_slab->next_task_slot.store(1, std::memory_order_relaxed);
slot = 0;
if (!tail_.compare_exchange_strong(slab, new_slab)) {
    // Another thread won the race, retry with their slab
    new_slab->prev = nullptr;
    DeleteSlab(new_slab);
    continue;
}
```

### Consumer Thread Workflow

The main thread in `Run()` performs:

1. **Task Draining**: `PopNextImmediateTask()` to get next task
2. **Delayed Task Processing**: Check for expired delayed tasks
3. **File Descriptor Polling**: Handle I/O events with fairness
4. **Task Execution**: Run tasks with watchdog protection

In the current design the run-loop performs one poll() per task. This is
arguably optimizable: if we know that we have a burst of tasks, we could run
them back-to-back without wasting syscall time on a poll(timeout=0).

Of course that would require some limit, to prevent livelocks in the case that
a (badly designed) function keeps re-posting itself until a socket has received
data (which would require a FD watch task to fire off).

Unfortunately, however, through the years our tests have accumulated
dependencies on the strict fairness of the legacy UnixTaskRunner. They expect to
be able to tell through `IsIdleForTesting()` if there is any upcoming FD watch
on the event horizon. As Hyrum's Law teaches, this is now an API of our
TaskRunner and will be until several tests get rewritten and de-flaked.


#### Task Consumption Algorithm

`PopTaskRecursive()` implements the consumption logic:

* It walks back the list of Slabs using recursion (in practice only going back
  by one Slab in nominal conditions).
* It scans all the bits in the `task_written` bitmap, and ANDs them with
  the `task_read` bitmap to extract unconsumed tasks in order.
* If all the tasks are read, it proceeds with the deletion of the Slab (more below).

```cpp
std::function<void()> PopTaskRecursive(Slab* slab, Slab* next_slab) {
    // First, recursively check older slabs (FIFO ordering)
    Slab* prev = slab->prev;
    if (prev) {
        auto task = PopTaskRecursive(prev, slab);
        if (task) return task;
    }
    
    // Then check current slab for published tasks
    for (size_t w = 0; w < Slab::kNumWords; ++w) {
        BitWord wr_word = slab->tasks_written[w].load(std::memory_order_acquire);
        BitWord rd_word = slab->tasks_read[w];
        BitWord unread_word = wr_word & ~rd_word;
        // Find and consume first unread task...
    }
    
    // Safe slab deletion logic...
}
```

### Reference Counting System

At first glance, the simple access pattern, where writers only access the tail
Slab and never walk back the list, greatly simplifies the need for complex
synchronization primitives. However there is one subtle race that needs to be
considered that requires some complication.

Consider the following scenario where two writer threads are invoking
`PostTask()` and the reader is simultaneously running and deleting a slab.

**Initial conditions**:

The task runner contains only one Slab S0, which happens to be full:
`tail_ -> S0 (full) -> nullptr`

**Race**:

* Thread A reads the `tail_` pointer and reads the address of S0. Before
  proceeding with the atomic increment of `next_task_slot` (which will disclose
  that the Slab is full) it gets pre-empted, suspending for a bit.
  ```cpp
  slab = tail_.load();
  // Pre-emption happens here
  slab->next_task_slot.fetch_add(1);
  ...
  ```

* Thread B does the same, but doesn't get pre-empted. So it reads S0, figures
  out it is full, allocates a new Slab S1 and replaces the tail.
  Thread B is happy and now:
  `tail_ -> S1 -> S0 -> nullptr`

* The Run() thread starts looping. It notices that there are two slabs, it
  notices that S0 is full, is NOT the tail and hence safe (!) to delete.

* At this point Thread A resumes its execution, tries to increment the
  S0->`next_task_slot`, but S0 has been deleted, causing a use-after-free.


**What is causing this race?**

It is true that it is safe to delete a non-tail Slab, as writers do not traverse
the linked list. However, a thread might have observed a non-tail Slab when it
happened to be the tail, and the reader thread has no way to know.

Adding a refcount (or any other property) to the Slab itself is useless, because
it doesn't solve the key problem that the Slab might be gone. The mitigation needs
to happen outside of the Slab.

In an intermediate design of LockFreeTaskRunner, `shared_ptr<Slab>` was used to
mitigate this. The non-intrusive STL `shared_ptr` introduces an intermediate
control block which decouples the Slab from its refcount.
Unfortunately, it turns out that libcxx implements the atomic accessors
to shared_ptr (required to swap the `shared_ptr<Slab> tail_` from different
threads) with a hashed pool of 32 mutexes, in practice defeating our lock-free
intentions (see [`__get_sp_mut`][__get_sp_mut]).

[__get_sp_mut]: https://github.com/llvm/llvm-project/blob/249167a8982afc3f55237baf1532c5c8ebd850b3/libcxx/src/memory.cpp#L123

**Initial simplistic mitigation**:

An initial simplistic mitigation approach would be the following: imagine every
writer increments a global refcount (e.g. `task_runner.num_writers_active`)
before starting and decreases it after having finished their PostTask().
This would allow the reader to know if, at any point in time, any writers are
active.

On the reader side we could skip the deletion of a slab - and try again on the
next task - if `num_writers_active > 0`. Note that this is NOT a mutex, nor
a spinlock, as nobody waits for anybody else.
It is based on this principle:

* Writers can only observe a Slab through the `tail_` pointer.
* When the reader decides to delete a slab, it deletes only non-tail Slabs, so
  it knows that the `tail_` points to a slab different than the one being
  deleted.
* If no writers are active, nobody could have observed any Slab, yet alone the
  Slab being deleted.
* If a writer becomes active immediately after the `num_writers_active > 0`
  check, it will necessarily observe the new tail Slab (assuming _sequential
  consistency_) and cannot observe the older Slab being deleted.

Now, while this would solve our race, it would expose us to a problematic
scenario: if a writer thread happens to be posting tasks every time the Run()
gets to that check, we might never be able to delete slabs.

To be honest, this scenario is quite unrealistic: if writers are always active,
we are likely going to explode the task runner, assuming tasks take more time
to run than what it takes to call PostTask().

**Current mitigation**:

In principle we would want a refcount for each Slab. But, as discussed before, the
refcount cannot live on the Slab itself, because it's used to gate the access
to the slab.

We could hold a refcount-per-slab in the task runner, using a
`map<Slab*, atomic<int>>` but that will cause heap churn, and also require a
lock-free map.

What we opted for is a middle-ground solution: we have a fixed number (32) of
refcount buckets and map each Slab to a bucket via a hash function.

Of course, two slabs could end up sharing the same refcount, creating a false
positive: we might think a Slab is refcounted even when it's not, due to
a hash collision.
But false positives, in this context, are harmless. In the absolutely worst case
we degenerate to the simplistic mitigation described above, which is still
correct from a race-viewpoint.

In practice we end up dividing the probability of deferring a Slab deletion
by 32x.

This is the logic that underpins the `LockFreeTaskRunner.refcounts_` array of
atomic integers, and the `ScopedRefCount` class used by the writers.


### Delayed Task Handling

Delayed tasks use a separate `FlatSet<DelayedTask>` container. This requires
some cost to maintain the entries sorted (we expect only a handful of delayed
tasks, as they are mostly used for timeouts), but avoids allocations in most
cases (FlatSet is based on a vector and allocates only if growth is necessary).

On the other hand, the reverse-ordering allows Run to pull tasks in O(1).
