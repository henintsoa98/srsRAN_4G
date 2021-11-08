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

#ifndef SRSRAN_RRC_NR_TEST_HELPERS_H
#define SRSRAN_RRC_NR_TEST_HELPERS_H

#include "srsenb/test/common/dummy_classes_common.h"
#include "srsgnb/hdr/stack/common/test/dummy_nr_classes.h"
#include "srsgnb/hdr/stack/rrc/rrc_nr.h"

namespace srsenb {

class pdcp_nr_rrc_tester : public pdcp_dummy
{
public:
  void write_sdu(uint16_t rnti, uint32_t lcid, srsran::unique_byte_buffer_t sdu, int pdcp_sn = -1) override
  {
    last_sdu_rnti = rnti;
    last_sdu_lcid = lcid;
    last_sdu      = std::move(sdu);
  }

  uint16_t                     last_sdu_rnti = SRSRAN_INVALID_RNTI;
  uint32_t                     last_sdu_lcid = srsran::MAX_NR_NOF_BEARERS;
  srsran::unique_byte_buffer_t last_sdu;
};

class rlc_nr_rrc_tester : public rlc_dummy
{
public:
  void write_sdu(uint16_t rnti, uint32_t lcid, srsran::unique_byte_buffer_t sdu)
  {
    last_sdu_rnti = rnti;
    last_sdu_lcid = lcid;
    last_sdu      = std::move(sdu);
  }

  uint16_t                     last_sdu_rnti;
  uint32_t                     last_sdu_lcid;
  srsran::unique_byte_buffer_t last_sdu;
};

/**
 * Run TS 38.331, 5.3.3 "RRC connection establishment" to completion
 * RRC actions:
 * - Rx RRCSetupRequest
 * - Tx RRCSetup to lower layers
 * - Tx RRCSetupComplete
 * Checks:
 * - the RRC sends RRCSetup as reply to RRCSetupRequest
 * - verify that RRCSetup rnti, lcid are correct
 * - verify that RRCSetup adds an SRB1
 */
void test_rrc_nr_connection_establishment(srsran::task_scheduler& task_sched,
                                          rrc_nr&                 rrc_obj,
                                          rlc_nr_rrc_tester&      rlc,
                                          uint16_t                rnti);

} // namespace srsenb

#endif // SRSRAN_RRC_NR_TEST_HELPERS_H