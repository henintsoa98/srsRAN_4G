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

#include "srsenb/hdr/stack/mac/nr/sched_nr_worker.h"

namespace srsenb {
namespace sched_nr_impl {

/// Called at the beginning of TTI in a locked context, to reserve available UE resources
void slot_cc_worker::start(tti_point tti_rx_, ue_map_t& ue_db)
{
  srsran_assert(not running(), "scheduler worker::start() called for active worker");
  tti_rx = tti_rx_;

  // Try reserve UE cells for this worker
  for (auto& ue_pair : ue_db) {
    uint16_t rnti = ue_pair.first;
    ue&      u    = *ue_pair.second;

    slot_ues.insert(rnti, u.try_reserve(tti_rx, cfg.cc));
    if (slot_ues[rnti].empty()) {
      // Failed to synchronize because UE is being used by another worker
      slot_ues.erase(rnti);
      continue;
    }
    // UE acquired successfully for scheduling in this {tti, cc}
  }

  res_grid.new_tti(tti_rx_);
  tti_rx = tti_rx_;
}

void slot_cc_worker::run()
{
  srsran_assert(running(), "scheduler worker::run() called for non-active worker");

  // Prioritize PDCCH scheduling for DL and UL data in a RoundRobin fashion
  if ((tti_rx.to_uint() & 0x1u) == 0) {
    alloc_dl_ues();
    alloc_ul_ues();
  } else {
    alloc_ul_ues();
    alloc_dl_ues();
  }

  // Select the winner PDCCH allocation combination, store all the scheduling results
  res_grid.generate_dcis();
}

void slot_cc_worker::end_tti()
{
  srsran_assert(running(), "scheduler worker::end() called for non-active worker");

  // releases UE resources
  slot_ues.clear();

  tti_rx = {};
}

void slot_cc_worker::alloc_dl_ues()
{
  if (slot_ues.empty()) {
    return;
  }
  slot_ue& ue = slot_ues.begin()->second;
  if (ue.h_dl == nullptr) {
    return;
  }

  rbgmask_t dlmask(cfg.cell_cfg.nof_rbg);
  dlmask.fill(0, dlmask.size(), true);
  res_grid.alloc_pdsch(ue, dlmask);
}
void slot_cc_worker::alloc_ul_ues()
{
  if (slot_ues.empty()) {
    return;
  }
  slot_ue& ue = slot_ues.begin()->second;
  if (ue.h_ul == nullptr) {
    return;
  }

  rbgmask_t ulmask(cfg.cell_cfg.nof_rbg);
  ulmask.fill(0, ulmask.size(), true);
  res_grid.alloc_pusch(ue, ulmask);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

sched_worker_manager::sched_worker_manager(ue_map_t& ue_db_, const sched_params& cfg_) : cfg(cfg_), ue_db(ue_db_)
{
  for (uint32_t cc = 0; cc < cfg.cells.size(); ++cc) {
    for (auto& slot_grid : phy_grid[cc]) {
      slot_grid = phy_slot_grid(cfg.cells[cc]);
    }
  }

  // Note: For now, we only allow parallelism at the sector level
  slot_ctxts.resize(cfg.sched_cfg.nof_concurrent_subframes);
  for (size_t i = 0; i < cfg.sched_cfg.nof_concurrent_subframes; ++i) {
    slot_ctxts[i].reset(new slot_worker_ctxt());
    sem_init(&slot_ctxts[i]->sf_sem, 0, 1);
    slot_ctxts[i]->workers.reserve(cfg.cells.size());
    for (uint32_t cc = 0; cc < cfg.cells.size(); ++cc) {
      slot_ctxts[i]->workers.emplace_back(cfg.cells[cc], phy_grid[cc]);
    }
  }
}

sched_worker_manager::~sched_worker_manager()
{
  // acquire all slot worker contexts
  for (auto& slot_ctxt : slot_ctxts) {
    sem_wait(&slot_ctxt->sf_sem);
  }
  // destroy all slot worker contexts
  for (auto& slot_ctxt : slot_ctxts) {
    sem_destroy(&slot_ctxt->sf_sem);
  }
}

sched_worker_manager::slot_worker_ctxt& sched_worker_manager::get_sf(tti_point tti_rx)
{
  return *slot_ctxts[tti_rx.to_uint() % slot_ctxts.size()];
}

void sched_worker_manager::reserve_workers(tti_point tti_rx_)
{
  // lock if slot worker is already being used
  auto& sf_worker_ctxt = get_sf(tti_rx_);
  sem_wait(&sf_worker_ctxt.sf_sem);

  sf_worker_ctxt.tti_rx = tti_rx_;
  sf_worker_ctxt.worker_count.store(static_cast<int>(sf_worker_ctxt.workers.size()), std::memory_order_relaxed);
}

void sched_worker_manager::start_tti(tti_point tti_rx_)
{
  auto& sf_worker_ctxt = get_sf(tti_rx_);
  srsran_assert(sf_worker_ctxt.tti_rx == tti_rx_, "invalid run_tti(tti, cc) arguments");

  for (uint32_t cc = 0; cc < sf_worker_ctxt.workers.size(); ++cc) {
    sf_worker_ctxt.workers[cc].start(sf_worker_ctxt.tti_rx, ue_db);
  }
}

bool sched_worker_manager::run_tti(tti_point tti_rx_, uint32_t cc, slot_res_t& tti_req)
{
  auto& sf_worker_ctxt = get_sf(tti_rx_);
  srsran_assert(sf_worker_ctxt.tti_rx == tti_rx_, "invalid run_tti(tti, cc) arguments");

  // Get {tti, cc} scheduling decision
  sf_worker_ctxt.workers[cc].run();

  // Copy requested TTI DL and UL sched result
  tti_req.dl_res.pdsch_tti = tti_rx_ + TX_ENB_DELAY;
  tti_req.dl_res.pdsch     = phy_grid[cc][tti_req.dl_res.pdsch_tti.to_uint()].pdsch_grants;
  tti_req.ul_res.pusch_tti = tti_rx_ + TX_ENB_DELAY;
  tti_req.ul_res.pusch     = phy_grid[cc][tti_req.ul_res.pusch_tti.to_uint()].pusch_grants;

  // decrement the number of active workers
  int rem_workers = sf_worker_ctxt.worker_count.fetch_sub(1, std::memory_order_release) - 1;
  srsran_assert(rem_workers >= 0, "invalid number of calls to run_tti(tti, cc)");

  if (rem_workers == 0) {
    // Clear one slot of PHY grid, so it can be reused in the next TTIs
    phy_grid[cc][sf_worker_ctxt.tti_rx.to_uint()].reset();
  }
  return rem_workers == 0;
}

void sched_worker_manager::end_tti(tti_point tti_rx_)
{
  auto& sf_worker_ctxt = get_sf(tti_rx_);
  srsran_assert(sf_worker_ctxt.tti_rx == tti_rx_, "invalid run_tti(tti, cc) arguments");
  srsran_assert(sf_worker_ctxt.worker_count == 0, "invalid number of calls to run_tti(tti, cc)");

  // All the workers of the same TTI have finished. Synchronize scheduling decisions with UEs state
  for (slot_cc_worker& worker : sf_worker_ctxt.workers) {
    worker.end_tti();
  }

  sem_post(&sf_worker_ctxt.sf_sem);
}

} // namespace sched_nr_impl
} // namespace srsenb