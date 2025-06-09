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

#include "src/base/intrusive_list.h"

#include "perfetto/base/logging.h"

namespace perfetto::base::internal {

void ListOps::PushFront(internal::ListNode* node) {
  PERFETTO_DCHECK(node->prev_ == 0 && node->next_ == 0);
  node->prev_ = sentinel();
  node->next_ = head_and_tail_.next_;
  head_and_tail_.next_ = reinterpret_cast<uintptr_t>(node);
  MaybeHeadAndTail(node->next_)->prev_ = reinterpret_cast<uintptr_t>(node);
}

void ListOps::PushBack(internal::ListNode* node) {
  PERFETTO_DCHECK(node->prev_ == 0 && node->next_ == 0);
  node->next_ = sentinel();
  node->prev_ = head_and_tail_.prev_;
  head_and_tail_.prev_ = reinterpret_cast<uintptr_t>(node);
  MaybeHeadAndTail(node->prev_)->next_ = reinterpret_cast<uintptr_t>(node);
}

void ListOps::InsertBefore(uintptr_t other_addr, internal::ListNode* node) {
  PERFETTO_DCHECK(node->prev_ == 0 && node->next_ == 0);
  internal::ListNode* other = MaybeHeadAndTail(other_addr);
  PERFETTO_DCHECK(other->prev_ != 0 && other->next_ != 0);
  uintptr_t prev_addr = other->prev_;
  internal::ListNode* prev = MaybeHeadAndTail(prev_addr);
  prev->next_ = reinterpret_cast<uintptr_t>(node);
  node->prev_ = prev_addr;
  node->next_ = other_addr;
  other->prev_ = reinterpret_cast<uintptr_t>(node);
}

void ListOps::PopFront() {
  PERFETTO_DCHECK(!empty());
  internal::ListNode* front = reinterpret_cast<ListNode*>(head_and_tail_.next_);
  head_and_tail_.next_ = front->next_;
  MaybeHeadAndTail(head_and_tail_.next_)->prev_ = sentinel();
  front->next_ = front->prev_ = 0;
}

void ListOps::PopBack() {
  PERFETTO_DCHECK(!empty());
  internal::ListNode* back = reinterpret_cast<ListNode*>(head_and_tail_.prev_);
  head_and_tail_.prev_ = back->prev_;
  MaybeHeadAndTail(head_and_tail_.prev_)->next_ = sentinel();
  back->next_ = back->prev_ = 0;
}

// static
void ListOps::Erase(internal::ListNode* node) {
  PERFETTO_DCHECK(node->prev_ && node->next_);
  auto* prev = MaybeHeadAndTail(node->prev_);
  auto* next = MaybeHeadAndTail(node->next_);
  prev->next_ = node->next_;
  next->prev_ = node->prev_;
  node->prev_ = node->next_ = 0;
}

}  // namespace perfetto::base::internal
