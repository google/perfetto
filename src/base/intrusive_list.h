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

#ifndef SRC_BASE_INTRUSIVE_LIST_H_
#define SRC_BASE_INTRUSIVE_LIST_H_

#include <cstddef>
#include <cstdint>

#include "perfetto/base/logging.h"

// An intrusive (doubly linked) list implementation.
// Unlike std::list<>, the entries being inserted into the list need to
// explicitly declare an IntrusiveListNode structure (one for each list they are
// part of). The user must specify a Traits struct for each list the entry is
// part of. The traits struct defines how to get to the IntrusiveListNode from
// the outer object.
//
// Usage example:
// class Person {
//  public:
//   struct ListTraits {
//     static constexpr size_t node_offset() { return offsetof(Person, node); }
//   };
//   std::string name;
//   IntrusiveListNode node{};
// }
//
// IntrusiveList<Person, Person::ListTraits> list;
// Person person;
// list.PushBack(person);
// ...

namespace perfetto::base {

namespace internal {

// ListNode is used both in:
// 1. actual list nodes.
// 2. as the list head_and_tail, point to the first and last element in the
//   list (or to itself, if the list is empty).
// When prev_/next_ point to an actual node, they contain the plain address
// (so it can just be reinterpret_cast-ed)
// When prev_/next_ point to the list head_and_tail, the address has the LSB set
// to 1 (which would othewise always be 0 due to pointer alignment).
// Doing so serves different purporses:
// - Identify the list head to stop the iterator.
// - Prevent bugs which try to dereference the list head casting it into a T.
//   (it causes a SIGBUS due to address misalignment on ARM)
// - Being able to detect when we reach the end of the list while iterating on
//   the node.next(), without having to have knowledge of the list object.
struct ListNode {
  uintptr_t prev_ = 0;
  uintptr_t next_ = 0;
};

// This function masks away the LSB returning a pointer to a ListNode. This can
// be used when we want to dereference a prev_/next_ pointer and we acknowledge
// that we might be operating on the head/tail (sentinel()) rather than a node.
// This is the symmetric of sentinel() (below).
inline ListNode* MaybeHeadAndTail(uintptr_t p) {
  return reinterpret_cast<ListNode*>(p & ~uintptr_t(1));
}

// IntrusiveList's Base class to factor out type-independent code (avoid binary
// bloat)
class ListOps {
 public:
  void PushFront(ListNode* node);
  void PushBack(ListNode* node);
  void InsertBefore(uintptr_t other_addr, ListNode* node);
  void PopFront();
  void PopBack();
  static void Erase(ListNode* node);
  bool empty() const { return head_and_tail_.next_ == sentinel(); }

  // Returns a pointer to the head_and_tail_ node, with the LSB set to 1.
  // See comments on ListNode about the sentinel.
  uintptr_t sentinel() const {
    return reinterpret_cast<uintptr_t>(&head_and_tail_) | 1;
  }
  ListNode head_and_tail_{sentinel(), sentinel()};
};

}  // namespace internal

// This is the public-facing type that clients get to see when they access
// MyObject.list_node. It essentially hides the next_ and prev_ raw pointers.
struct IntrusiveListNode : private internal::ListNode {
  // Returns true if the element is NOT part of a list (never added or removed).
  bool is_attached() const {
    PERFETTO_DCHECK((next_ == 0 && prev_ == 0) || (next_ != 0 && prev_ != 0));
    return next_ != 0;
  }
};

// T is the class that has one or more IntrusiveListNode as fields.
// Traits defines getter and offset between node and T.
// Traits is separate to allow the same T to be part of different lists (which
// necessitate a different Traits, at very least for the offset).
template <typename T, typename ListTraits = typename T::ListTraits>
class IntrusiveList : private internal::ListOps {
 public:
  class Iterator {
   public:
    explicit Iterator(uintptr_t node) : node_(node) {
      PERFETTO_DCHECK(node != 0);
    }

    explicit Iterator(T* entry)
        : Iterator(reinterpret_cast<uintptr_t>(nodeof(entry))) {}

    ~Iterator() = default;
    Iterator(const Iterator&) = default;
    Iterator& operator=(const Iterator&) = default;
    Iterator(Iterator&&) noexcept = default;
    Iterator& operator=(Iterator&&) noexcept = default;

    explicit operator bool() const { return (node_ & 1) == 0; }

    bool operator==(const Iterator& other) const {
      return node_ == other.node_;
    }
    bool operator!=(const Iterator& other) const { return !(*this == other); }

    T* operator->() {
      PERFETTO_DCHECK(operator bool());
      return const_cast<T*>(
          entryof(reinterpret_cast<internal::ListNode*>(node_)));
    }
    T& operator*() { return *operator->(); }

    Iterator& operator++() {
      node_ = static_cast<uintptr_t>(internal::MaybeHeadAndTail(node_)->next_);
      PERFETTO_DCHECK(node_);
      return *this;
    }

    Iterator& operator--() {
      node_ = static_cast<uintptr_t>(internal::MaybeHeadAndTail(node_)->prev_);
      PERFETTO_DCHECK(node_);
      return *this;
    }

    // Erases the current node and moves to the next one (or to end()).
    Iterator& Erase() {
      PERFETTO_DCHECK(*this);
      auto* cur = reinterpret_cast<internal::ListNode*>(node_);
      ++(*this);  // Move the iterator before erasing so it stays valid.
      internal::ListOps::Erase(cur);
      return *this;
    }

   private:
    friend class IntrusiveList;
    uintptr_t node_;
  };

  using value_type = T;
  using const_pointer = const T*;

  void PushFront(T& entry) { internal::ListOps::PushFront(nodeof(&entry)); }
  void PushBack(T& entry) { internal::ListOps::PushBack(nodeof(&entry)); }
  void InsertBefore(Iterator it, T& entry) {
    internal::ListOps::InsertBefore(it.node_, nodeof(&entry));
  }

  void PopFront() { internal::ListOps::PopFront(); }
  void PopBack() { internal::ListOps::PopBack(); }

  T& front() {
    PERFETTO_DCHECK((head_and_tail_.next_ & 1) == 0);
    return const_cast<T&>(
        *entryof(reinterpret_cast<internal::ListNode*>(head_and_tail_.next_)));
  }

  T& back() {
    PERFETTO_DCHECK((head_and_tail_.prev_ & 1) == 0);
    return const_cast<T&>(
        *entryof(reinterpret_cast<internal::ListNode*>(head_and_tail_.prev_)));
  }

  void Erase(T& entry) { internal::ListOps::Erase(nodeof(&entry)); }

  bool empty() const { return internal::ListOps::empty(); }

  Iterator begin() { return Iterator(head_and_tail_.next_); }
  Iterator end() { return Iterator(sentinel()); }

  Iterator rbegin() { return Iterator(head_and_tail_.prev_); }
  Iterator rend() { return Iterator(sentinel()); }

  // Obtains back a List from an iterator. It is okay to pass a falsy iterator
  // (i.e. end() / an iterator that was incremented past the last valid entry).
  static IntrusiveList<T, ListTraits>* FromIterator(Iterator it) {
    // Rewind the iterator until we reach the head_and_tail.
    for (; it; --it) {
    }
    uintptr_t ht_ptr =
        reinterpret_cast<uintptr_t>(internal::MaybeHeadAndTail(it.node_));
    using ListType = IntrusiveList<T, ListTraits>;
    return reinterpret_cast<ListType*>(ht_ptr -
                                       offsetof(ListType, head_and_tail_));
  }

 private:
  static constexpr size_t kNodeOffset = ListTraits::node_offset();

  static constexpr internal::ListNode* nodeof(T* entry) {
    return reinterpret_cast<internal::ListNode*>(
        reinterpret_cast<uintptr_t>(entry) + kNodeOffset);
  }

  static constexpr const T* entryof(internal::ListNode* node) {
    return reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(node) -
                                kNodeOffset);
  }
};

}  // namespace perfetto::base

#endif  // SRC_BASE_INTRUSIVE_LIST_H_
