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

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;

class Person {
 public:
  struct Traits {
    static constexpr size_t node_offset() { return offsetof(Person, node); }
  };

  // For ASSERT_EQ/EXPECT_EQ.
  bool operator==(const Person& p) const { return name == p.name; }

  std::string name;
  IntrusiveListNode node{};
};

class IntrusiveListTest : public ::testing::Test {
 protected:
  void AssertListValues(const std::vector<Person>& expected) {
    auto it = list_.begin();

    for (const auto& e : expected) {
      ASSERT_TRUE(it);
      ASSERT_EQ(e, *it);
      ++it;
    }

    EXPECT_FALSE(it);
    EXPECT_EQ(it, list_.end());
  }

  Person p1_{"a"};
  Person p2_{"b"};
  Person p3_{"c"};
  Person p4_{"d"};

  using PersonList = IntrusiveList<Person, Person::Traits>;
  PersonList list_;
};

TEST_F(IntrusiveListTest, PushFront) {
  AssertListValues({});
  ASSERT_EQ(list_.begin(), list_.end());

  list_.PushFront(p3_);
  AssertListValues({p3_});
  ASSERT_EQ(&*list_.begin(), &p3_);
  ASSERT_EQ(&list_.front(), &p3_);
  ASSERT_EQ(&list_.back(), &p3_);

  list_.PushFront(p2_);
  AssertListValues({p2_, p3_});
  ASSERT_EQ(&*list_.begin(), &p2_);
  ASSERT_EQ(&list_.front(), &p2_);
  ASSERT_EQ(&list_.back(), &p3_);

  list_.PushFront(p1_);
  AssertListValues({p1_, p2_, p3_});
  ASSERT_EQ(&*list_.begin(), &p1_);
  ASSERT_EQ(&list_.front(), &p1_);
  ASSERT_EQ(&list_.back(), &p3_);
}

TEST_F(IntrusiveListTest, PushBack) {
  AssertListValues({});

  list_.PushBack(p1_);
  AssertListValues({p1_});

  list_.PushBack(p2_);
  AssertListValues({p1_, p2_});

  list_.PushBack(p3_);
  AssertListValues({p1_, p2_, p3_});
}

TEST_F(IntrusiveListTest, PushFrontAndBack) {
  AssertListValues({});

  list_.PushFront(p2_);
  AssertListValues({p2_});

  list_.PushBack(p3_);
  AssertListValues({p2_, p3_});

  list_.PushFront(p1_);
  AssertListValues({p1_, p2_, p3_});
}

TEST_F(IntrusiveListTest, PopFrontAndBack) {
  list_.PushBack(p1_);
  list_.PushBack(p2_);
  list_.PushBack(p3_);
  AssertListValues({p1_, p2_, p3_});

  ASSERT_EQ(list_.front(), p1_);
  list_.PopBack();
  ASSERT_EQ(list_.front(), p1_);
  ASSERT_EQ(list_.back(), p2_);

  list_.PopFront();
  ASSERT_EQ(list_.front(), p2_);
  ASSERT_EQ(list_.back(), p2_);

  list_.PopBack();
  ASSERT_TRUE(list_.empty());
  ASSERT_EQ(list_.begin(), list_.end());
}

TEST_F(IntrusiveListTest, InsertBefore) {
  // InsertBefore(end()) on empty list.
  list_.InsertBefore(list_.begin(), p1_);
  AssertListValues({p1_});
  list_.Erase(p1_);

  // InsertBefore(end()) on empty list.
  list_.InsertBefore(list_.end(), p1_);
  AssertListValues({p1_});
  list_.Erase(p1_);

  // InsertBefore(rend()) on empty list.
  list_.InsertBefore(list_.rbegin(), p1_);
  AssertListValues({p1_});
  list_.Erase(p1_);

  // InsertBefore(rend()) on empty list.
  list_.InsertBefore(list_.rend(), p1_);
  AssertListValues({p1_});
  list_.Erase(p1_);

  // InserBefore a valid element.
  list_.PushBack(p2_);
  list_.PushBack(p4_);
  list_.InsertBefore(--list_.rend(), p3_);
  AssertListValues({p2_, p3_, p4_});
  list_.InsertBefore(list_.begin(), p1_);
  AssertListValues({p1_, p2_, p3_, p4_});
}

TEST_F(IntrusiveListTest, Erase) {
  list_.PushFront(p4_);
  list_.PushFront(p3_);
  list_.PushFront(p2_);
  list_.PushFront(p1_);

  AssertListValues({p1_, p2_, p3_, p4_});

  list_.Erase(p2_);
  AssertListValues({p1_, p3_, p4_});

  list_.Erase(p1_);
  AssertListValues({p3_, p4_});

  list_.Erase(p4_);
  AssertListValues({p3_});

  list_.Erase(p3_);
  AssertListValues({});

  // Now insert again.
  list_.PushFront(p4_);
  list_.PushFront(p2_);
  AssertListValues({p2_, p4_});
}

TEST_F(IntrusiveListTest, Empty) {
  ASSERT_TRUE(list_.empty());

  list_.PushFront(p2_);
  ASSERT_FALSE(list_.empty());

  list_.PushFront(p1_);
  ASSERT_FALSE(list_.empty());

  list_.PopFront();
  ASSERT_FALSE(list_.empty());

  list_.PopFront();
  ASSERT_TRUE(list_.empty());
}

TEST_F(IntrusiveListTest, Iteration) {
  list_.PushFront(p3_);
  list_.PushFront(p2_);
  list_.PushFront(p1_);

  auto it = list_.begin();
  ASSERT_EQ(*it, p1_);

  ++it;
  ASSERT_EQ(*it, p2_);

  ++it;
  ASSERT_EQ(*it, p3_);

  ++it;
  ASSERT_EQ(it, list_.end());
}

TEST_F(IntrusiveListTest, IterationBackwards) {
  list_.PushFront(p3_);
  list_.PushFront(p2_);
  list_.PushFront(p1_);

  auto it = list_.rbegin();
  ASSERT_EQ(*it, p3_);

  --it;
  ASSERT_EQ(*it, p2_);

  --it;
  ASSERT_EQ(*it, p1_);

  --it;
  ASSERT_EQ(it, list_.rend());
}

TEST_F(IntrusiveListTest, RangeBasedForLoop) {
  list_.PushFront(p3_);
  list_.PushFront(p2_);
  list_.PushFront(p1_);

  std::vector<const Person*> looped_persons;

  for (const auto& p : list_) {
    looped_persons.push_back(&p);
  }

  ASSERT_THAT(looped_persons, ::testing::ElementsAre(&p1_, &p2_, &p3_));
}

TEST_F(IntrusiveListTest, IteratorOps) {
  list_.PushFront(p3_);
  list_.PushFront(p2_);
  list_.PushFront(p1_);

  ASSERT_EQ(PersonList::Iterator(&p1_)->name, "a");
  ASSERT_EQ(PersonList::Iterator(&p2_)->name, "b");
  ASSERT_EQ(PersonList::Iterator(&p3_)->name, "c");

  ASSERT_FALSE((--PersonList::Iterator(&p1_)));

  ASSERT_EQ((--PersonList::Iterator(&p2_))->name, "a");
  ASSERT_EQ((++PersonList::Iterator(&p2_))->name, "c");

  ASSERT_FALSE((++PersonList::Iterator(&p3_)));
}

TEST_F(IntrusiveListTest, IteratorErase) {
  list_.PushFront(p3_);
  list_.PushFront(p2_);
  list_.PushFront(p1_);

  auto it = PersonList::Iterator(&p1_);
  ASSERT_TRUE(it->node.is_attached());
  ASSERT_EQ(it->name, "a");
  it.Erase();  // `it` now points at p2.
  AssertListValues({p2_, p3_});
  ASSERT_EQ(&*it, &p2_);
  ASSERT_EQ(--it, list_.end());
  ASSERT_EQ(&*(++it), &p2_);
  ASSERT_EQ(&*(++it), &p3_);
}

TEST_F(IntrusiveListTest, ListFromIterator) {
  PersonList list1, list2;

  list1.PushBack(p1_);
  list1.PushBack(p2_);

  list2.PushBack(p3_);
  list2.PushBack(p4_);

  ASSERT_EQ(PersonList::FromIterator(PersonList::Iterator(&p1_)), &list1);
  ASSERT_EQ(PersonList::FromIterator(PersonList::Iterator(&p2_)), &list1);
  ASSERT_EQ(PersonList::FromIterator(list1.begin()), &list1);
  ASSERT_EQ(PersonList::FromIterator(list1.end()), &list1);

  ASSERT_EQ(PersonList::FromIterator(PersonList::Iterator(&p3_)), &list2);
  ASSERT_EQ(PersonList::FromIterator(PersonList::Iterator(&p4_)), &list2);
  ASSERT_EQ(PersonList::FromIterator(list2.begin()), &list2);
  ASSERT_EQ(PersonList::FromIterator(list2.end()), &list2);
}
}  // namespace
}  // namespace base
}  // namespace perfetto
