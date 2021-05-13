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
#include "srsue/hdr/phy/scell/intra_measure_lte.h"

namespace srsue {
namespace scell {

#define Error(fmt, ...)                                                                                                \
  if (SRSRAN_DEBUG_ENABLED)                                                                                            \
  logger.error("INTRA-%s: " fmt, to_string(get_rat()).c_str(), ##__VA_ARGS__)
#define Warning(fmt, ...)                                                                                              \
  if (SRSRAN_DEBUG_ENABLED)                                                                                            \
  logger.warning("INTRA-%s: " fmt, to_string(get_rat()).c_str(), ##__VA_ARGS__)
#define Info(fmt, ...)                                                                                                 \
  if (SRSRAN_DEBUG_ENABLED)                                                                                            \
  logger.info("INTRA-%s: " fmt, to_string(get_rat()).c_str(), ##__VA_ARGS__)
#define Debug(fmt, ...)                                                                                                \
  if (SRSRAN_DEBUG_ENABLED)                                                                                            \
  logger.debug("INTRA-%s: " fmt, to_string(get_rat()).c_str(), ##__VA_ARGS__)

intra_measure_lte::intra_measure_lte(srslog::basic_logger& logger_, meas_itf& new_cell_itf_) :
  logger(logger_), scell_rx(logger_), intra_measure_base(logger_, new_cell_itf_)
{}

intra_measure_lte::~intra_measure_lte()
{
  scell_rx.deinit();
  srsran_refsignal_dl_sync_free(&refsignal_dl_sync);
}

void intra_measure_lte::init(uint32_t cc_idx, const args_t& args)
{
  init_generic(cc_idx, args);

  // Initialise Reference signal measurement
  srsran_refsignal_dl_sync_init(&refsignal_dl_sync);

  // Start scell
  scell_rx.init(args.len_ms);
}

void intra_measure_lte::set_primary_cell(uint32_t earfcn, srsran_cell_t cell)
{
  current_earfcn = earfcn;
  serving_cell   = cell;
  set_current_sf_len((uint32_t)SRSRAN_SF_LEN_PRB(cell.nof_prb));
}

void intra_measure_lte::measure_rat(const measure_context_t& context, std::vector<cf_t>& buffer)
{
  std::set<uint32_t> cells_to_measure = context.active_pci;

  // Detect new cells using PSS/SSS
  std::set<uint32_t> detected_cells = scell_rx.find_cells(buffer.data(), serving_cell, context.meas_len_ms);

  // Add detected cells to the list of cells to measure
  for (const uint32_t& c : detected_cells) {
    cells_to_measure.insert(c);
  }

  // Initialise empty neighbour cell list
  std::vector<phy_meas_t> neighbour_cells = {};

  context.new_cell_itf.cell_meas_reset(context.cc_idx);

  // Use Cell Reference signal to measure cells in the time domain for all known active PCI
  for (const uint32_t& id : cells_to_measure) {
    // Do not measure serving cell here since it's measured by workers
    if (id == serving_cell.id) {
      continue;
    }
    srsran_cell_t cell = serving_cell;
    cell.id            = id;

    srsran_refsignal_dl_sync_set_cell(&refsignal_dl_sync, cell);
    srsran_refsignal_dl_sync_run(&refsignal_dl_sync, buffer.data(), context.meas_len_ms * context.sf_len);

    if (refsignal_dl_sync.found) {
      phy_meas_t m = {};
      m.rat        = srsran::srsran_rat_t::lte;
      m.pci        = cell.id;
      m.earfcn     = current_earfcn;
      m.rsrp       = refsignal_dl_sync.rsrp_dBfs - context.rx_gain_offset_db;
      m.rsrq       = refsignal_dl_sync.rsrq_dB;
      m.cfo_hz     = refsignal_dl_sync.cfo_Hz;
      neighbour_cells.push_back(m);

      Info("Found neighbour cell: EARFCN=%d, PCI=%03d, RSRP=%5.1f dBm, RSRQ=%5.1f, peak_idx=%5d, "
           "CFO=%+.1fHz",
           m.earfcn,
           m.pci,
           m.rsrp,
           m.rsrq,
           refsignal_dl_sync.peak_index,
           refsignal_dl_sync.cfo_Hz);
    }
  }

  // Send measurements to RRC if any cell found
  if (not neighbour_cells.empty()) {
    context.new_cell_itf.new_cell_meas(context.cc_idx, neighbour_cells);
  }
}

} // namespace scell
} // namespace srsue