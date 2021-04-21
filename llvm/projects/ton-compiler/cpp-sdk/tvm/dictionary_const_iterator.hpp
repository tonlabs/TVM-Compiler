#pragma once

#include <tvm/dict_traits.hpp>
#include <tvm/schema/chain_fold.hpp>

namespace tvm {

template<class Element, unsigned KeyLen>
struct small_dictionary_const_iterator {
  using iterator_category = std::forward_iterator_tag;
  using value_type = Element;
  using difference_type = int;
  using pointer = Element*;
  using reference = Element&;

  dictionary dict_;
  unsigned idx_;
  std::optional<slice> sl_;

  __always_inline Element operator*() const {
    return schema::parse<Element>(*sl_);
  }

  __always_inline bool is_end() const { return !sl_; }

  static small_dictionary_const_iterator create_begin(dictionary dict) {
    std::optional<slice> opt_sl;
    auto [sl, idx, succ] = dict.dictumin(KeyLen);
    if (succ)
      opt_sl = sl;
    return small_dictionary_const_iterator{dict, idx, opt_sl};
  }
  static small_dictionary_const_iterator create_end(dictionary dict) {
    return small_dictionary_const_iterator{{}, 0, {}};
  }

  __always_inline small_dictionary_const_iterator operator++() {
    require(!!sl_, error_code::iterator_overflow);
    auto [sl, next_idx, succ] = dict_.dictugetnext(idx_, KeyLen);
    if (succ) {
      idx_ = next_idx;
      sl_ = sl;
    } else {
      sl_.reset();
    }
    return *this;
  }
  bool operator==(small_dictionary_const_iterator v) const {
    bool left_end = is_end();
    bool right_end = v.is_end();
    return left_end && right_end;
  }
  bool operator!=(small_dictionary_const_iterator v) const {
    return !(*this == v);
  }
};

template<class Element, unsigned KeyLen>
struct big_dictionary_const_iterator {
  using iterator_category = std::forward_iterator_tag;
  using value_type = Element;
  using difference_type = int;
  using pointer = Element*;
  using reference = Element&;

  dictionary dict_;
  unsigned idx_;
  std::optional<cell> cl_;

  __always_inline Element operator*() const {
    using namespace schema;
    using data_tup_t = to_std_tuple_t<Element>;
    using LinearTup = decltype(make_chain_tuple(data_tup_t{}));
    auto linear_tup = parse<LinearTup>(parser(*cl_));
    return to_struct<Element>(chain_fold_tup<data_tup_t>(linear_tup));
  }

  __always_inline bool is_end() const { return !cl_; }

  __always_inline
  static big_dictionary_const_iterator create_begin(dictionary dict) {
    std::optional<cell> opt_cl;
    auto [cl, idx, succ] = dict.dictuminref(KeyLen);
    if (succ)
      opt_cl = cl;
    return big_dictionary_const_iterator{dict, idx, opt_cl};
  }
  __always_inline
  static big_dictionary_const_iterator create_end(dictionary dict) {
    return big_dictionary_const_iterator{{}, 0, {}};
  }

  __always_inline big_dictionary_const_iterator operator++() {
    require(!!cl_, error_code::iterator_overflow);
    auto [sl, next_idx, succ] = dict_.dictugetnext(idx_, KeyLen);
    if (succ) {
      idx_ = next_idx;
      cl_ = parser(sl).ldref();
    } else {
      cl_.reset();
    }
    return *this;
  }
  __always_inline
  bool operator==(big_dictionary_const_iterator v) const {
    bool left_end = is_end();
    bool right_end = v.is_end();
    return left_end && right_end;
  }
  __always_inline
  bool operator!=(big_dictionary_const_iterator v) const {
    return !(*this == v);
  }
};

template<class Element, unsigned KeyLen>
using dictionary_const_iterator =
  std::conditional_t<small_element<Element, KeyLen>,
                     small_dictionary_const_iterator<Element, KeyLen>,
                     big_dictionary_const_iterator<Element, KeyLen>>;

} // namespace tvm

