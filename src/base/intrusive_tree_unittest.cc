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

#include "src/base/intrusive_tree.h"

#include <random>
#include <set>

#include "perfetto/ext/base/fnv_hash.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;

class Person {
 public:
  struct Traits {
    using KeyType = std::string;
    static constexpr size_t NodeOffset() { return offsetof(Person, node); }
    static const std::string& GetKey(const Person& p) { return p.name; }
  };

  // For ASSERT_THAT.
  bool operator==(const Person& p) const { return name == p.name; }

  std::string name;
  IntrusiveTreeNode node{};
};

TEST(IntrusiveTreeTest, InsertionAndRemoval) {
  IntrusiveTree<Person, Person::Traits> tree;
  Person p1{"a"};
  Person p2{"b"};
  Person p3{"c"};

  {
    auto [it, inserted] = tree.Insert(p1);
    ASSERT_TRUE(inserted);
    ASSERT_EQ(*it, p1);
  }

  {
    auto [it, inserted] = tree.Insert(p3);
    ASSERT_TRUE(inserted);
    ASSERT_EQ(*it, p3);
  }

  {
    auto [it, inserted] = tree.Insert(p2);
    ASSERT_TRUE(inserted);
    ASSERT_EQ(*it, p2);
  }

  // Inserting the same node again should fail.
  {
    auto [it, inserted] = tree.Insert(p1);
    ASSERT_FALSE(inserted);
    ASSERT_EQ(*it, p1);
  }

  ASSERT_EQ(*tree.Find("a"), p1);
  ASSERT_EQ(*tree.Find("b"), p2);
  ASSERT_EQ(*tree.Find("c"), p3);
  ASSERT_FALSE(tree.Find("0_notfound"));
  ASSERT_FALSE(tree.Find("a_"));
  ASSERT_FALSE(tree.Find("b_"));
  ASSERT_FALSE(tree.Find("c_"));
  ASSERT_FALSE(tree.Find("z_notfound"));

  auto it_p2 = tree.Remove(tree.begin());
  ASSERT_EQ(*it_p2, p2);
  ASSERT_FALSE(tree.Find("a"));

  auto it_end = tree.Remove(p3);
  ASSERT_EQ(it_end, tree.end());
  ASSERT_FALSE(tree.Find("c"));

  ASSERT_TRUE(tree.Remove("b"));
  ASSERT_FALSE(tree.Find("b"));
}

TEST(IntrusiveTreeTest, Iterator) {
  IntrusiveTree<Person, Person::Traits> tree;

  ASSERT_EQ(tree.begin(), tree.end());

  Person p1{"a"};
  ASSERT_TRUE(tree.Insert(p1).second);
  auto it = tree.begin();
  ASSERT_NE(it, tree.end());
  ASSERT_EQ(it->name, "a");
  ASSERT_EQ(++it, tree.end());

  Person p2{"b"};
  Person p3{"c"};
  ASSERT_TRUE(tree.Insert(p2).second);
  ASSERT_TRUE(tree.Insert(p3).second);

  it = tree.begin();
  ASSERT_NE(it, tree.end());
  ASSERT_EQ(it->name, "a");

  ASSERT_NE(++it, tree.end());
  ASSERT_EQ(it->name, "b");

  ASSERT_NE(++it, tree.end());
  ASSERT_EQ(it->name, "c");

  ASSERT_EQ(++it, tree.end());

  ASSERT_THAT(tree, ElementsAre(p1, p2, p3));
}

TEST(IntrusiveTreeTest, Size) {
  Person p1{"a"};
  Person p2{"b"};

  IntrusiveTree<Person, Person::Traits> tree;
  ASSERT_EQ(tree.Size(), static_cast<size_t>(0));

  tree.Insert(p1);
  ASSERT_EQ(tree.Size(), static_cast<size_t>(1));

  tree.Insert(p2);
  ASSERT_EQ(tree.Size(), static_cast<size_t>(2));

  tree.Remove("c");
  ASSERT_EQ(tree.Size(), static_cast<size_t>(2));

  tree.Remove("a");
  ASSERT_EQ(tree.Size(), static_cast<size_t>(1));

  tree.Remove(p2);
  ASSERT_EQ(tree.Size(), static_cast<size_t>(0));
}

class IdEntry {
 public:
  struct Traits {
    using KeyType = uint64_t;
    static constexpr size_t NodeOffset() { return offsetof(IdEntry, node); }
    static uint64_t GetKey(const IdEntry& p) { return p.id; }
  };
  bool operator<(const IdEntry& o) const { return id < o.id; }
  bool operator==(const IdEntry& o) const {
    return id == o.id && hash == o.hash;
  }

  uint64_t id;
  uint64_t hash;
  IntrusiveTreeNode node{};
};

// Compare the behavior of IntrusiveTree vs std::set.
TEST(IntrusiveTreeTest, Golden) {
  IntrusiveTree<IdEntry, IdEntry::Traits> tree;
  std::set<IdEntry> std_set;
  std::minstd_rand0 rnd_engine(0);
  static constexpr size_t N = 10000;
  std::vector<IdEntry> storage;
  storage.resize(N);

  for (size_t n = 0; n < N; n++) {
    IdEntry& entry = storage[n];
    entry.id = static_cast<uint64_t>(rnd_engine());
    entry.hash = base::FnvHash<uint64_t>()(entry.id);
    auto res_std_set = std_set.emplace(entry);
    auto res_tree = tree.Insert(entry);
    ASSERT_EQ(res_std_set.second, res_tree.second);
    if (res_std_set.second) {
      ASSERT_EQ(*res_std_set.first, *res_tree.first);
    }
  }

  EXPECT_THAT(tree, ElementsAreArray(std_set.begin(), std_set.end()));

  // Remove random elements
  for (auto it = std_set.begin(); it != std_set.end();) {
    auto next = it;
    next++;
    if (rnd_engine() % 4 == 0) {
      tree.Remove(it->id);
      std_set.erase(it);
    }
    it = next;
  }
  EXPECT_THAT(tree, ElementsAreArray(std_set.begin(), std_set.end()));
}

}  // namespace
}  // namespace base
}  // namespace perfetto
