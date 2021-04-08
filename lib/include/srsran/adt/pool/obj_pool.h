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

#ifndef SRSRAN_OBJ_POOL_H
#define SRSRAN_OBJ_POOL_H

#include "batch_mem_pool.h"
#include "memblock_cache.h"
#include "pool_interface.h"

namespace srsran {

template <typename T>
class background_obj_pool;

template <typename T>
class growing_batch_obj_pool final : public obj_pool_itf<T>
{
  static size_t memblock_size()
  {
    /// Node Structure [ node header | (pad to node alignment) | node size | (pad to node header alignment) ]
    return align_next(align_next(free_memblock_list::min_memblock_size(), alignof(T)) + sizeof(T),
                      free_memblock_list::min_memblock_align());
  }
  static size_t batch_size(size_t nof_objs_per_batch)
  {
    /// Batch Structure: [allocated stack header | (pad max alignment) | [memblock] x objs_per_batch ]
    return align_next(detail::max_alignment + (memblock_size() * nof_objs_per_batch), detail::max_alignment);
  }

public:
  using init_mem_oper_t = srsran::move_callback<void(void*)>;
  using recycle_oper_t  = srsran::move_callback<void(T&)>;

  explicit growing_batch_obj_pool(size_t          objs_per_batch_,
                                  int             init_size     = -1,
                                  init_mem_oper_t init_oper_    = detail::inplace_default_ctor_operator<T>{},
                                  recycle_oper_t  recycle_oper_ = detail::noop_operator{}) :
    objs_per_batch(objs_per_batch_),
    init_oper(std::move(init_oper_)),
    recycle_oper(std::move(recycle_oper_)),
    allocated(batch_size(objs_per_batch_), detail::max_alignment),
    cache(sizeof(T), alignof(T))
  {
    size_t N = init_size < 0 ? objs_per_batch_ : init_size;
    while (N > cache.size()) {
      allocate_batch();
    }
  }
  ~growing_batch_obj_pool() { clear(); }

  void clear()
  {
    if (not allocated.empty()) {
      srsran_assert(allocated.size() * objs_per_batch == cache_size(),
                    "Not all objects have been deallocated (%zd < %zd)",
                    cache_size(),
                    allocated.size() * objs_per_batch);
      while (not cache.empty()) {
        void* node_payload = cache.top();
        static_cast<T*>(node_payload)->~T();
        cache.pop();
      }
      allocated.clear();
    }
  }

  void allocate_batch()
  {
    uint8_t* batch_payload = static_cast<uint8_t*>(allocated.allocate_block());
    for (size_t i = 0; i < objs_per_batch; ++i) {
      void* cache_node = batch_payload + (i * cache.memblock_size);
      cache.push(cache_node);
      init_oper(cache.top());
    }
  }

  size_t cache_size() const { return cache.size(); }

private:
  friend class background_obj_pool<T>;

  T* do_allocate() final
  {
    if (cache.empty()) {
      allocate_batch();
    }
    void* top = cache.top();
    cache.pop();
    return static_cast<T*>(top);
  }

  void do_deallocate(void* payload_ptr) final
  {
    recycle_oper(*static_cast<T*>(payload_ptr));
    void* header_ptr = cache.get_node_header(payload_ptr);
    cache.push(header_ptr);
  }

  // args
  const size_t    objs_per_batch;
  init_mem_oper_t init_oper;
  recycle_oper_t  recycle_oper;

  memblock_stack     allocated;
  memblock_node_list cache;
};

/**
 * Thread-safe object pool specialized in allocating batches of objects in a preemptive way in a background thread
 * to minimize latency.
 * Note: The dispatched allocation jobs may outlive the pool. To handle this, the pool state is passed to jobs via a
 *       shared ptr.
 */
template <typename T>
class background_obj_pool final : public obj_pool_itf<T>
{
public:
  using init_mem_oper_t = typename growing_batch_obj_pool<T>::init_mem_oper_t;
  using recycle_oper_t  = typename growing_batch_obj_pool<T>::recycle_oper_t;

  explicit background_obj_pool(size_t          nof_objs_per_batch,
                               size_t          thres_,
                               int             init_size     = -1,
                               init_mem_oper_t init_oper_    = detail::inplace_default_ctor_operator<T>{},
                               recycle_oper_t  recycle_oper_ = detail::noop_operator{}) :
    thres(thres_),
    state(std::make_shared<detached_pool_state>(this)),
    grow_pool(nof_objs_per_batch, init_size, std::move(init_oper_), std::move(recycle_oper_))
  {
    srsran_assert(thres_ > 1, "The provided threshold=%zd is not valid", thres_);
  }
  ~background_obj_pool()
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->pool = nullptr;
    grow_pool.clear();
  }

  size_t cache_size() const { return grow_pool.cache_size(); }

private:
  T* do_allocate() final
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    T*                          obj = grow_pool.do_allocate();
    if (grow_pool.cache_size() < thres) {
      allocate_batch_in_background_();
    }
    return obj;
  }
  void do_deallocate(void* ptr) final
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    return grow_pool.do_deallocate(ptr);
  }

  void allocate_batch_in_background_()
  {
    std::shared_ptr<detached_pool_state> state_copy = state;
    get_background_workers().push_task([state_copy]() {
      std::lock_guard<std::mutex> lock(state_copy->mutex);
      if (state_copy->pool != nullptr) {
        state_copy->pool->grow_pool.allocate_batch();
      }
    });
  }

  size_t thres;

  // state of pool is detached because pool may be destroyed while batches are being allocated in the background
  struct detached_pool_state {
    std::mutex              mutex;
    background_obj_pool<T>* pool;
    explicit detached_pool_state(background_obj_pool<T>* pool_) : pool(pool_) {}
  };
  std::shared_ptr<detached_pool_state> state;

  growing_batch_obj_pool<T> grow_pool;
};

} // namespace srsran

#endif // SRSRAN_OBJ_POOL_H