#include "cachex/recency_list.hpp"

#include <string>

#include "test_framework.hpp"

namespace {

std::string order_of(const cachex::RecencyList& list) {
  std::string out;
  for (const cachex::Entry& entry : list) {
    if (!out.empty()) {
      out += ",";
    }
    out += entry.key;
  }
  return out;
}

}  // namespace

CACHEX_TEST(list_starts_empty) {
  const cachex::RecencyList list;
  CHECK(list.empty());
  CHECK_EQ(list.size(), 0u);
}

CACHEX_TEST(inserts_go_to_the_front) {
  cachex::RecencyList list;
  list.insert_newest("a", "1");
  list.insert_newest("b", "2");
  list.insert_newest("c", "3");

  CHECK_EQ(order_of(list), "c,b,a");
  CHECK_EQ(list.size(), 3u);
}

CACHEX_TEST(insert_returns_an_iterator_to_the_new_entry) {
  cachex::RecencyList list;
  const auto it = list.insert_newest("key", "value");

  CHECK_EQ(it->key, "key");
  CHECK_EQ(it->value, "value");
}

CACHEX_TEST(mark_used_moves_an_entry_to_the_front) {
  cachex::RecencyList list;
  const auto a = list.insert_newest("a", "1");
  list.insert_newest("b", "2");
  list.insert_newest("c", "3");

  list.mark_used(a);
  CHECK_EQ(order_of(list), "a,c,b");
}

CACHEX_TEST(mark_used_on_the_front_entry_is_a_no_op) {
  cachex::RecencyList list;
  list.insert_newest("a", "1");
  const auto b = list.insert_newest("b", "2");

  list.mark_used(b);
  CHECK_EQ(order_of(list), "b,a");
  CHECK_EQ(list.size(), 2u);
}

CACHEX_TEST(mark_used_on_the_only_entry_is_a_no_op) {
  cachex::RecencyList list;
  const auto only = list.insert_newest("solo", "1");

  list.mark_used(only);
  CHECK_EQ(order_of(list), "solo");
  CHECK_EQ(list.size(), 1u);
}

CACHEX_TEST(mark_used_keeps_the_iterator_valid) {
  cachex::RecencyList list;
  const auto a = list.insert_newest("a", "1");
  list.insert_newest("b", "2");

  list.mark_used(a);
  list.mark_used(a);  // splice relinks the node; `a` must still point at it
  CHECK_EQ(a->key, "a");
  CHECK_EQ(a->value, "1");
}

CACHEX_TEST(erase_removes_only_the_named_entry) {
  cachex::RecencyList list;
  list.insert_newest("a", "1");
  const auto b = list.insert_newest("b", "2");
  list.insert_newest("c", "3");

  list.erase(b);
  CHECK_EQ(order_of(list), "c,a");
  CHECK_EQ(list.size(), 2u);
}

CACHEX_TEST(oldest_reports_the_back_of_the_list) {
  cachex::RecencyList list;
  const auto a = list.insert_newest("a", "1");
  list.insert_newest("b", "2");
  list.insert_newest("c", "3");

  CHECK_EQ(list.oldest().key, "a");

  // Using "a" promotes it, so the oldest becomes the next one along.
  list.mark_used(a);
  CHECK_EQ(list.oldest().key, "b");
}

CACHEX_TEST(pop_oldest_removes_from_the_back) {
  cachex::RecencyList list;
  list.insert_newest("a", "1");
  list.insert_newest("b", "2");
  list.insert_newest("c", "3");

  list.pop_oldest();
  CHECK_EQ(order_of(list), "c,b");
  CHECK_EQ(list.oldest().key, "b");

  list.pop_oldest();
  list.pop_oldest();
  CHECK(list.empty());
}

CACHEX_TEST(clear_empties_the_list) {
  cachex::RecencyList list;
  list.insert_newest("a", "1");
  list.insert_newest("b", "2");

  list.clear();
  CHECK(list.empty());
  CHECK_EQ(list.size(), 0u);
}

// The property the whole design rests on: an iterator held elsewhere (the hash
// map, in Cache) must survive every operation on *other* entries. If this were
// not true -- std::vector, for instance -- the map would be full of dangling
// references after the first reallocation.
CACHEX_TEST(iterators_survive_insertions_and_erasures_of_other_entries) {
  cachex::RecencyList list;
  list.insert_newest("a", "1");
  const auto tracked = list.insert_newest("tracked", "value");
  const auto c = list.insert_newest("c", "3");

  for (int i = 0; i < 500; ++i) {
    list.insert_newest("filler" + std::to_string(i), "x");
  }
  list.erase(c);
  list.pop_oldest();  // removes "a"

  CHECK_EQ(tracked->key, "tracked");
  CHECK_EQ(tracked->value, "value");

  // And it is still a usable handle, not just readable memory.
  list.mark_used(tracked);
  CHECK_EQ(list.begin()->key, "tracked");
}

CACHEX_TEST(entries_store_independent_copies) {
  cachex::RecencyList list;
  std::string value = "original";
  const auto it = list.insert_newest("k", value);
  value = "changed-after-insert";

  CHECK_EQ(it->value, "original");
}
