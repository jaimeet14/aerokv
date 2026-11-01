#include "iterator.h"

#include <algorithm>

namespace aerokv {
namespace {

class VectorIterator final : public Iterator {
 public:
  explicit VectorIterator(std::vector<Entry> entries)
      : entries_(std::move(entries)), pos_(entries_.size()) {}

  bool Valid() const override { return pos_ < entries_.size(); }
  void SeekToFirst() override { pos_ = 0; }
  void Seek(std::string_view target) override {
    auto it = std::lower_bound(
        entries_.begin(), entries_.end(), target,
        [](const Entry& e, std::string_view t) { return std::string_view(e.key) < t; });
    pos_ = static_cast<size_t>(it - entries_.begin());
  }
  void Next() override { ++pos_; }
  std::string_view key() const override { return entries_[pos_].key; }
  std::string_view value() const override { return entries_[pos_].value; }
  ValueType type() const override { return entries_[pos_].type; }

 private:
  std::vector<Entry> entries_;
  size_t pos_;
};

class MergingIterator final : public Iterator {
 public:
  explicit MergingIterator(std::vector<std::unique_ptr<Iterator>> children)
      : children_(std::move(children)) {}

  bool Valid() const override { return !heap_.empty(); }

  void SeekToFirst() override {
    for (auto& c : children_) c->SeekToFirst();
    Rebuild();
  }

  void Seek(std::string_view target) override {
    for (auto& c : children_) c->Seek(target);
    Rebuild();
  }

  // Advances every child positioned on the current key, which skips the older
  // versions shadowed by the entry just returned.
  void Next() override {
    current_.assign(key());
    while (!heap_.empty() && children_[heap_.front()]->key() == current_) {
      const size_t i = heap_.front();
      std::pop_heap(heap_.begin(), heap_.end(), cmp_);
      heap_.pop_back();
      children_[i]->Next();
      if (children_[i]->Valid()) {
        heap_.push_back(i);
        std::push_heap(heap_.begin(), heap_.end(), cmp_);
      }
    }
  }

  std::string_view key() const override { return children_[heap_.front()]->key(); }
  std::string_view value() const override { return children_[heap_.front()]->value(); }
  ValueType type() const override { return children_[heap_.front()]->type(); }

  Status status() const override {
    for (const auto& c : children_) {
      Status s = c->status();
      if (!s.ok()) return s;
    }
    return Status::OK();
  }

 private:
  // Orders the heap so the front is the smallest key, ties going to the newest child.
  struct Greater {
    const std::vector<std::unique_ptr<Iterator>>* children;
    bool operator()(size_t a, size_t b) const {
      const int r = (*children)[a]->key().compare((*children)[b]->key());
      return r != 0 ? r > 0 : a > b;
    }
  };

  void Rebuild() {
    heap_.clear();
    for (size_t i = 0; i < children_.size(); ++i) {
      if (children_[i]->Valid()) heap_.push_back(i);
    }
    std::make_heap(heap_.begin(), heap_.end(), cmp_);
  }

  std::vector<std::unique_ptr<Iterator>> children_;
  std::vector<size_t> heap_;
  std::string current_;  // reused buffer: the key being skipped past in Next()
  Greater cmp_{&children_};
};

}  // namespace

std::unique_ptr<Iterator> NewVectorIterator(std::vector<Entry> sorted_entries) {
  return std::make_unique<VectorIterator>(std::move(sorted_entries));
}

std::unique_ptr<Iterator> NewMergingIterator(std::vector<std::unique_ptr<Iterator>> children) {
  return std::make_unique<MergingIterator>(std::move(children));
}

}  // namespace aerokv
