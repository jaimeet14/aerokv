#include "memtable.h"

#include <mutex>

namespace aerokv {

MemTable::MemTable(uint64_t log_number) : head_(new Node{}), log_number_(log_number) {
  for (auto& n : head_->next) n = nullptr;
}

MemTable::~MemTable() {
  Node* n = head_;
  while (n != nullptr) {
    Node* next = n->next[0];
    delete n;
    n = next;
  }
}

int MemTable::RandomHeight() {
  // Each level is kept with probability 1/4, giving expected O(log n) search.
  int h = 1;
  while (h < kMaxHeight) {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 7;
    rng_ ^= rng_ << 17;
    if ((rng_ & 3) != 0) break;
    ++h;
  }
  return h;
}

MemTable::Node* MemTable::FindGreaterOrEqual(std::string_view target, Node** prev) const {
  Node* x = head_;
  for (int level = height_ - 1; level >= 0; --level) {
    Node* next = x->next[level];
    while (next != nullptr && std::string_view(next->key) < target) {
      x = next;
      next = x->next[level];
    }
    if (prev != nullptr) prev[level] = x;
  }
  return x->next[0];
}

void MemTable::Add(std::string_view key, std::string_view value, ValueType type) {
  std::unique_lock<std::shared_mutex> lock(mu_);
  Node* prev[kMaxHeight];
  Node* x = FindGreaterOrEqual(key, prev);
  if (x != nullptr && x->key == key) {
    usage_.fetch_add(value.size(), std::memory_order_relaxed);
    usage_.fetch_sub(x->value.size(), std::memory_order_relaxed);
    x->value.assign(value.data(), value.size());
    x->type = type;
    return;
  }
  const int h = RandomHeight();
  if (h > height_) {
    for (int i = height_; i < h; ++i) prev[i] = head_;
    height_ = h;
  }
  Node* n = new Node{std::string(key), std::string(value), type, {}};
  for (int i = 0; i < h; ++i) {
    n->next[i] = prev[i]->next[i];
    prev[i]->next[i] = n;
  }
  for (int i = h; i < kMaxHeight; ++i) n->next[i] = nullptr;
  ++count_;
  usage_.fetch_add(sizeof(Node) + key.size() + value.size(), std::memory_order_relaxed);
}

bool MemTable::Get(std::string_view key, std::string* value, ValueType* type) const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  const Node* x = FindGreaterOrEqual(key, nullptr);
  if (x == nullptr || x->key != key) return false;
  *type = x->type;
  if (x->type == ValueType::kValue) value->assign(x->value);
  return true;
}

size_t MemTable::Count() const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  return count_;
}

class MemTable::Iter final : public Iterator {
 public:
  explicit Iter(std::shared_ptr<const MemTable> mem) : mem_(std::move(mem)), lock_(mem_->mu_) {}

  bool Valid() const override { return node_ != nullptr; }
  void SeekToFirst() override { node_ = mem_->head_->next[0]; }
  void Seek(std::string_view target) override {
    node_ = mem_->FindGreaterOrEqual(target, nullptr);
  }
  void Next() override { node_ = node_->next[0]; }
  std::string_view key() const override { return node_->key; }
  std::string_view value() const override { return node_->value; }
  ValueType type() const override { return node_->type; }

 private:
  std::shared_ptr<const MemTable> mem_;
  std::shared_lock<std::shared_mutex> lock_;
  const Node* node_ = nullptr;
};

std::unique_ptr<Iterator> MemTable::NewIterator() const {
  return std::make_unique<Iter>(shared_from_this());
}

}  // namespace aerokv
