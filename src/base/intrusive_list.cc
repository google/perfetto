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

void ListOps::PushFront(IntrusiveListNode* node) {
  PERFETTO_DCHECK(node->prev == nullptr && node->next == nullptr);
  node->prev = &head_;
  node->next = head_.next;
  head_.next = node;
  node->next->prev = node;
  ++size_;
}

void ListOps::PushBack(IntrusiveListNode* node) {
  PERFETTO_DCHECK(node->prev == nullptr && node->next == nullptr);
  node->next = &head_;
  node->prev = head_.prev;
  head_.prev = node;
  node->prev->next = node;
  ++size_;
}

void ListOps::PopFront() {
  PERFETTO_DCHECK(size_ > 0);
  IntrusiveListNode* front = head_.next;
  head_.next = front->next;
  head_.next->prev = &head_;
  front->next = front->prev = nullptr;
  --size_;
}

void ListOps::PopBack() {
  PERFETTO_DCHECK(size_ > 0);
  IntrusiveListNode* back = head_.prev;
  head_.prev = back->prev;
  head_.prev->next = &head_;
  back->next = back->prev = nullptr;
  --size_;
}

void ListOps::Erase(IntrusiveListNode* node) {
  PERFETTO_DCHECK(size_ > 0);
  auto* prev = node->prev;
  auto* next = node->next;
  prev->next = next;
  next->prev = prev;
  node->prev = node->next = nullptr;
  --size_;
}

}  // namespace perfetto::base::internal
