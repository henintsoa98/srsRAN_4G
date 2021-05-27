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

#include "srsran/phy/ch_estimation/dmrs_pbch.h"
#include "srsran/phy/common/sequence.h"
#include "srsran/phy/utils/debug.h"
#include "srsran/phy/utils/vector.h"
#include <complex.h>
#include <math.h>

/*
 * Number of NR PBCH DMRS resource elements present in an SSB resource grid
 */
#define DMRS_PBCH_NOF_RE 144

static uint32_t dmrs_pbch_cinit(const srsran_dmrs_pbch_cfg_t* cfg)
{
  // Default values for L_max == 4
  uint64_t i_ssb = (cfg->ssb_idx & 0b11U) + 4UL * cfg->n_hf; // Least 2 significant bits

  if (cfg->L_max == 8 || cfg->L_max == 64) {
    i_ssb = cfg->ssb_idx & 0b111U; // Least 3 significant bits
  }

  return SRSRAN_SEQUENCE_MOD(((i_ssb + 1UL) * (SRSRAN_FLOOR(cfg->N_id, 4UL) + 1UL) << 11UL) + ((i_ssb + 1UL) << 6UL) +
                             (cfg->N_id % 4));
}

int srsran_dmrs_pbch_put(const srsran_dmrs_pbch_cfg_t* cfg, cf_t ssb_grid[SRSRAN_SSB_NOF_RE])
{
  // Validate inputs
  if (cfg == NULL || ssb_grid == NULL) {
    return SRSRAN_ERROR_INVALID_INPUTS;
  }

  // Calculate index shift
  uint32_t v = cfg->N_id % 4;

  // Calculate power allocation
  float beta = M_SQRT1_2;
  if (isnormal(cfg->beta)) {
    beta = cfg->beta;
  }

  // Initialise sequence
  uint32_t                cinit          = dmrs_pbch_cinit(cfg);
  srsran_sequence_state_t sequence_state = {};
  srsran_sequence_state_init(&sequence_state, cinit);

  // Generate sequence
  cf_t r[DMRS_PBCH_NOF_RE];
  srsran_sequence_state_gen_f(&sequence_state, beta, (float*)r, DMRS_PBCH_NOF_RE * 2);

  // r sequence read index
  uint32_t r_idx = 0;

  // Put sequence in symbol 1
  for (uint32_t k = v; k < SRSRAN_SSB_BW_SUBC; k += 4) {
    ssb_grid[SRSRAN_SSB_BW_SUBC * 1 + k] = r[r_idx++];
  }

  // Put sequence in symbol 2, lower section
  for (uint32_t k = v; k < 48; k += 4) {
    ssb_grid[SRSRAN_SSB_BW_SUBC * 2 + k] = r[r_idx++];
  }

  // Put sequence in symbol 2, upper section
  for (uint32_t k = 192 + v; k < SRSRAN_SSB_BW_SUBC; k += 4) {
    ssb_grid[SRSRAN_SSB_BW_SUBC * 2 + k] = r[r_idx++];
  }

  // Put sequence in symbol 3
  for (uint32_t k = v; k < SRSRAN_SSB_BW_SUBC; k += 4) {
    ssb_grid[SRSRAN_SSB_BW_SUBC * 3 + k] = r[r_idx++];
  }

  return SRSRAN_SUCCESS;
}

int dmrs_pbch_extract_lse(const srsran_dmrs_pbch_cfg_t* cfg,
                          const cf_t                    ssb_grid[SRSRAN_SSB_NOF_RE],
                          cf_t                          lse[DMRS_PBCH_NOF_RE])
{
  // Calculate index shift
  uint32_t v = cfg->N_id % 4;

  // Calculate power allocation
  float beta = M_SQRT1_2;
  if (isnormal(cfg->beta)) {
    beta = cfg->beta;
  }

  // Initialise sequence
  uint32_t                cinit          = dmrs_pbch_cinit(cfg);
  srsran_sequence_state_t sequence_state = {};
  srsran_sequence_state_init(&sequence_state, cinit);

  // Generate sequence
  cf_t r[DMRS_PBCH_NOF_RE];
  srsran_sequence_state_gen_f(&sequence_state, beta, (float*)r, DMRS_PBCH_NOF_RE * 2);

  // r sequence read index
  uint32_t r_idx = 0;

  // Put sequence in symbol 1
  for (uint32_t k = v; k < SRSRAN_SSB_BW_SUBC; k += 4) {
    lse[r_idx++] = ssb_grid[SRSRAN_SSB_BW_SUBC * 1 + k];
  }

  // Put sequence in symbol 2, lower section
  for (uint32_t k = v; k < 48; k += 4) {
    lse[r_idx++] = ssb_grid[SRSRAN_SSB_BW_SUBC * 2 + k];
  }

  // Put sequence in symbol 2, upper section
  for (uint32_t k = 192 + v; k < SRSRAN_SSB_BW_SUBC; k += 4) {
    lse[r_idx++] = ssb_grid[SRSRAN_SSB_BW_SUBC * 2 + k];
  }

  // Put sequence in symbol 3
  for (uint32_t k = v; k < SRSRAN_SSB_BW_SUBC; k += 4) {
    lse[r_idx++] = ssb_grid[SRSRAN_SSB_BW_SUBC * 3 + k];
  }

  // Calculate actual least square estimates
  srsran_vec_prod_conj_ccc(lse, r, lse, DMRS_PBCH_NOF_RE);

  return SRSRAN_SUCCESS;
}

int srsran_dmrs_pbch_estimate(const srsran_dmrs_pbch_cfg_t* cfg,
                              const cf_t                    ssb_grid[SRSRAN_SSB_NOF_RE],
                              cf_t                          ce[SRSRAN_SSB_NOF_RE],
                              srsran_dmrs_pbch_meas_t*      meas)
{
  // Validate inputs
  if (cfg == NULL || ssb_grid == NULL || ce == NULL || meas == NULL) {
    return SRSRAN_ERROR_INVALID_INPUTS;
  }

  // Extract least square estimates
  cf_t lse[DMRS_PBCH_NOF_RE];
  if (dmrs_pbch_extract_lse(cfg, ssb_grid, lse) < SRSRAN_SUCCESS) {
    return SRSRAN_ERROR;
  }

  float scs_hz = SRSRAN_SUBC_SPACING_NR(cfg->scs);
  if (!isnormal(scs_hz)) {
    ERROR("Invalid SCS");
    return SRSRAN_ERROR;
  }

  // Compute average delay in microseconds from the symbols 1 and 3 (symbol 2 does not carry PBCH in all the grid)
  float avg_delay1_norm = srsran_vec_estimate_frequency(&lse[0], 60) / 4.0f;
  float avg_delay3_norm = srsran_vec_estimate_frequency(&lse[84], 60) / 4.0f;
  float avg_delay_norm  = (avg_delay1_norm + avg_delay3_norm) / 2.0f;
  float avg_delay_us    = avg_delay_norm / scs_hz;

  // Generate a second SSB grid with the corrected average delay
  cf_t ssb_grid_corrected[SRSRAN_SSB_NOF_RE];
  for (uint32_t l = 0; l < SRSRAN_SSB_DURATION_NSYMB; l++) {
    srsran_vec_apply_cfo(&ssb_grid[SRSRAN_SSB_BW_SUBC * l],
                         avg_delay_norm,
                         &ssb_grid_corrected[SRSRAN_SSB_BW_SUBC * l],
                         SRSRAN_SSB_BW_SUBC);
  }

  // Extract LSE from corrected grid
  if (dmrs_pbch_extract_lse(cfg, ssb_grid_corrected, lse) < SRSRAN_SUCCESS) {
    return SRSRAN_ERROR;
  }

  // Compute correlation of symbols 1 and 3
  cf_t corr1 = srsran_vec_acc_cc(&lse[0], 60) / 60.0f;
  cf_t corr3 = srsran_vec_acc_cc(&lse[84], 60) / 60.0f;

  // Estimate CFO from correlation
  float distance_s = srsran_symbol_distance_s(1, 3, cfg->scs);
  float cfo_hz     = 0.0f;
  if (isnormal(distance_s)) {
    cfo_hz = cargf(corr1 * conjf(corr3)) / (2.0f * (float)M_PI * distance_s);
  }

  // Estimate wideband gain at symbol 0
  cf_t wideband_gain = (srsran_vec_acc_cc(lse, DMRS_PBCH_NOF_RE) / DMRS_PBCH_NOF_RE) *
                       cexpf(I * 2.0f * M_PI * srsran_symbol_offset_s(2, cfg->scs) * cfo_hz);

  // Compute RSRP from correlation
  float rsrp = SRSRAN_CSQABS((corr1 + corr3) / 2.0f);

  // Compute EPRE
  float epre = srsran_vec_avg_power_cf(lse, DMRS_PBCH_NOF_RE);

  // Write measurements
  meas->corr         = rsrp / epre;
  meas->epre         = epre;
  meas->rsrp         = rsrp;
  meas->cfo_hz       = cfo_hz;
  meas->avg_delay_us = avg_delay_us;

  // Compute channel estimates
  for (uint32_t l = 0; l < SRSRAN_SSB_DURATION_NSYMB; l++) {
    float t_s                  = srsran_symbol_offset_s(l, cfg->scs);
    cf_t  symbol_wideband_gain = cexpf(-I * 2.0f * M_PI * cfo_hz * t_s) * wideband_gain;
    srsran_vec_gen_sine(symbol_wideband_gain, -avg_delay_norm, &ce[l * SRSRAN_SSB_BW_SUBC], SRSRAN_SSB_BW_SUBC);
  }

  return SRSRAN_SUCCESS;
}