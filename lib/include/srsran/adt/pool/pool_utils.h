/**
 *
 * \section COPYRIGHT
 *
 * Copyright 2013-2021 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#ifndef SRSRAN_POOL_UTILS_H
#define SRSRAN_POOL_UTILS_H

#include "../move_callback.h"
#include <memory>

namespace srsran {

namespace detail {

template <typename T>
struct inplace_default_ctor_operator {
  void operator()(void* ptr) { new (ptr) T(); }
};

struct noop_operator {
  template <typename T>
  void operator()(const T& ptr)
  {
    // do nothing
  }
};

} // namespace detail

} // namespace srsran

#endif // SRSRAN_POOL_UTILS_H