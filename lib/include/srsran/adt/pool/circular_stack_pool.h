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

#ifndef SRSRAN_CIRCULAR_MAP_STACK_POOL_H
#define SRSRAN_CIRCULAR_MAP_STACK_POOL_H

#include "batch_mem_pool.h"
#include "linear_allocator.h"
#include "srsran/adt/circular_array.h"
#include <mutex>

namespace srsran {

template <size_t NofStacks>
class circular_stack_pool
{
  struct mem_block_elem_t {
    std::mutex       mutex;
    size_t           key   = std::numeric_limits<size_t>::max();
    size_t           count = 0;
    linear_allocator alloc;

    void clear()
    {
      key   = std::numeric_limits<size_t>::max();
      count = 0;
      alloc.clear();
    }
  };

public:
  circular_stack_pool(size_t nof_objs_per_batch, size_t stack_size, size_t batch_thres, int initial_size = -1) :
    central_cache(std::min(NofStacks, nof_objs_per_batch), stack_size, batch_thres, initial_size),
    logger(srslog::fetch_basic_logger("POOL"))
  {}
  circular_stack_pool(circular_stack_pool&&)      = delete;
  circular_stack_pool(const circular_stack_pool&) = delete;
  circular_stack_pool& operator=(circular_stack_pool&&) = delete;
  circular_stack_pool& operator=(const circular_stack_pool&) = delete;
  ~circular_stack_pool()
  {
    for (mem_block_elem_t& elem : pools) {
      std::unique_lock<std::mutex> lock(elem.mutex);
      srsran_assert(elem.count == 0, "There are missing deallocations for stack id=%zd", elem.key);
      if (elem.alloc.is_init()) {
        void* ptr = elem.alloc.memblock_ptr();
        elem.alloc.clear();
        central_cache.deallocate_node(ptr);
      }
    }
  }

  void* allocate(size_t key, size_t size, size_t alignment) noexcept
  {
    size_t                       idx  = key % NofStacks;
    mem_block_elem_t&            elem = pools[idx];
    std::unique_lock<std::mutex> lock(elem.mutex);
    if (not elem.alloc.is_init()) {
      void* block = central_cache.allocate_node(central_cache.get_node_max_size());
      if (block == nullptr) {
        logger.warning("Failed to allocate memory block from central cache");
        return nullptr;
      }
      elem.key   = key;
      elem.alloc = linear_allocator(block, central_cache.get_node_max_size());
    }
    void* ptr = elem.alloc.allocate(size, alignment);
    if (ptr == nullptr) {
      logger.warning("No space left in memory block with key=%zd of circular stack pool", key);
    } else {
      elem.count++;
    }
    return ptr;
  }

  void deallocate(size_t key, void* p)
  {
    size_t                      idx  = key % NofStacks;
    mem_block_elem_t&           elem = pools[idx];
    std::lock_guard<std::mutex> lock(elem.mutex);
    elem.alloc.deallocate(p);
    elem.count--;
    if (elem.count == 0) {
      // return back to central cache
      void* ptr = elem.alloc.memblock_ptr();
      elem.clear();
      central_cache.deallocate_node(ptr);
    }
  }

  void allocate_batch() { central_cache.allocate_batch(); }

  size_t cache_size() const { return central_cache.cache_size(); }

private:
  srsran::circular_array<mem_block_elem_t, NofStacks> pools;
  srsran::background_mem_pool                         central_cache;
  srslog::basic_logger&                               logger;
};

} // namespace srsran

#endif // SRSRAN_CIRCULAR_MAP_STACK_POOL_H