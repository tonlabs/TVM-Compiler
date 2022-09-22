#pragma once

#include <optional>
#include <tvm/builder.hpp>
#include <tvm/error_code.hpp>
#include <tvm/assert.hpp>
#include <tvm/schema/basics.hpp>

namespace tvm {

/// tuple_array allows to store supported elements in array in tvm tuple
template<class T>
class tuple_array {
};

/// Implementation of tuple_array for cell elements
template<>
class tuple_array<cell> {
public:
  /// Creates empty array/tuple
  tuple_array() : tup_(__builtin_tvm_tuple()) {}

  template<typename ...Args>
  tuple_array(Args... args) : tup_(__builtin_tvm_tuple(args...)) {}

  /// Returns true if array is empty
  bool empty() const {
    return size() == 0;
  }

  /// Returns array size
  unsigned size() const { return __builtin_tvm_tlen(tup_); }

  /// Push new value at the end
  void push_back(cell val) {
    tup_ = __builtin_tvm_tpush(tup_, __builtin_tvm_cast_from_cell(val.get()));
  }

  /// Pop value from the end
  void pop_back() {
    auto [tup, val] = __builtin_tvm_tpop(tup_);
    tup_ = tup;
  }

  /// Pop value from the begin
  void pop_front() {
    auto [val, tup] = __builtin_tvm_tpopfront(tup_, size());
    tup_ = tup;
  }

  /// Returns array element #idx
  cell operator[](unsigned idx) const {
    return __builtin_tvm_cast_to_cell(__builtin_tvm_index(tup_, idx));
  }
  /// Sets array element #idx
  void set_at(unsigned idx, cell val) {
    tup_ = __builtin_tvm_setindex(tup_, __builtin_tvm_cast_from_cell(val), idx);
  }
  /// Erase array element #idx
  void erase_at(unsigned idx) {
    unsigned sz = size();
    tup_ = __builtin_tvm_trerase(sz - idx, tup_, sz);
  }
private:
  __tvm_tuple tup_;
};

} // namespace tvm

