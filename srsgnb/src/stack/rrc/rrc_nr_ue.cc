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

#include "srsgnb/hdr/stack/rrc/rrc_nr_ue.h"
#include "srsgnb/hdr/stack/rrc/cell_asn1_config.h"
#include "srsran/asn1/rrc_nr_utils.h"
#include "srsran/common/string_helpers.h"

using namespace asn1::rrc_nr;

namespace srsenb {

/*******************************************************************************
  UE class

Every function in UE class is called from a mutex environment thus does not
    need extra protection.
    *******************************************************************************/
rrc_nr::ue::ue(rrc_nr* parent_, uint16_t rnti_, const sched_nr_ue_cfg_t& uecfg_, bool start_msg3_timer) :
  parent(parent_), rnti(rnti_), uecfg(uecfg_)
{
  // Derive UE cfg from rrc_cfg_nr_t
  uecfg.phy_cfg.pdcch = parent->cfg.cell_list[0].phy_cell.pdcch;

  // Set timer for MSG3_RX_TIMEOUT or UE_INACTIVITY_TIMEOUT
  activity_timer = parent->task_sched.get_unique_timer();
  start_msg3_timer ? set_activity_timeout(MSG3_RX_TIMEOUT) : set_activity_timeout(MSG5_RX_TIMEOUT);
}

rrc_nr::ue::~ue() {}

void rrc_nr::ue::set_activity_timeout(activity_timeout_type_t type)
{
  uint32_t deadline_ms = 0;

  switch (type) {
    case MSG3_RX_TIMEOUT:
      // TODO: Retrieve the parameters from somewhere(RRC?) - Currently hardcoded to 100ms
      deadline_ms = 100;
      break;
    case MSG5_RX_TIMEOUT:
      // TODO: Retrieve the parameters from somewhere(RRC?) - Currently hardcoded to 1s
      deadline_ms = 5000;
      break;
    case UE_INACTIVITY_TIMEOUT:
      // TODO: Retrieve the parameters from somewhere(RRC?) - Currently hardcoded to 5s
      deadline_ms = 10000;
      break;
    default:
      parent->logger.error("Unknown timeout type %d", type);
      return;
  }

  activity_timer.set(deadline_ms, [this, type](uint32_t tid) { activity_timer_expired(type); });
  parent->logger.debug("Setting timer for %s for rnti=0x%x to %dms", to_string(type).c_str(), rnti, deadline_ms);

  set_activity();
}

void rrc_nr::ue::set_activity(bool enabled)
{
  if (not enabled) {
    if (activity_timer.is_running()) {
      parent->logger.debug("Inactivity timer interrupted for rnti=0x%x", rnti);
    }
    activity_timer.stop();
    return;
  }

  // re-start activity timer with current timeout value
  activity_timer.run();
  parent->logger.debug("Activity registered for rnti=0x%x (timeout_value=%dms)", rnti, activity_timer.duration());
}

void rrc_nr::ue::activity_timer_expired(const activity_timeout_type_t type)
{
  parent->logger.info("Activity timer for rnti=0x%x expired after %d ms", rnti, activity_timer.time_elapsed());

  switch (type) {
    case MSG5_RX_TIMEOUT:
    case UE_INACTIVITY_TIMEOUT:
      state = rrc_nr_state_t::RRC_INACTIVE;
      parent->rrc_eutra->sgnb_inactivity_timeout(eutra_rnti);
      break;
    case MSG3_RX_TIMEOUT: {
      // MSG3 timeout, no need to notify NGAP or LTE stack. Just remove UE
      state                = rrc_nr_state_t::RRC_IDLE;
      uint32_t rnti_to_rem = rnti;
      parent->task_sched.defer_task([this, rnti_to_rem]() { parent->rem_user(rnti_to_rem); });
      break;
    }
    default:
      // Unhandled activity timeout, just remove UE and log an error
      parent->rem_user(rnti);
      parent->logger.error(
          "Unhandled reason for activity timer expiration. rnti=0x%x, cause %d", rnti, static_cast<unsigned>(type));
  }
}

std::string rrc_nr::ue::to_string(const activity_timeout_type_t& type)
{
  constexpr static const char* options[] = {"Msg3 reception", "UE inactivity", "Msg5 reception"};
  return srsran::enum_to_text(options, (uint32_t)activity_timeout_type_t::nulltype, (uint32_t)type);
}

void rrc_nr::ue::send_dl_ccch(const dl_ccch_msg_s& dl_ccch_msg)
{
  // Allocate a new PDU buffer, pack the message and send to PDCP
  srsran::unique_byte_buffer_t pdu = parent->pack_into_pdu(dl_ccch_msg);
  if (pdu == nullptr) {
    parent->logger.error("Failed to send DL-CCCH");
    return;
  }
  fmt::memory_buffer fmtbuf;
  fmt::format_to(fmtbuf, "DL-CCCH.{}", dl_ccch_msg.msg.c1().type().to_string());
  log_rrc_message(srsran::nr_srb::srb0, Tx, *pdu.get(), dl_ccch_msg, srsran::to_c_str(fmtbuf));
  parent->rlc->write_sdu(rnti, srsran::srb_to_lcid(srsran::nr_srb::srb0), std::move(pdu));
}

void rrc_nr::ue::send_dl_dcch(srsran::nr_srb srb, const asn1::rrc_nr::dl_dcch_msg_s& dl_dcch_msg)
{
  // Allocate a new PDU buffer, pack the message and send to PDCP
  srsran::unique_byte_buffer_t pdu = parent->pack_into_pdu(dl_dcch_msg);
  if (pdu == nullptr) {
    parent->logger.error("Failed to send DL-DCCH");
    return;
  }
  fmt::memory_buffer fmtbuf;
  fmt::format_to(fmtbuf, "DL-DCCH.{}", dl_dcch_msg.msg.c1().type().to_string());
  log_rrc_message(srb, Tx, *pdu.get(), dl_dcch_msg, srsran::to_c_str(fmtbuf));
  parent->pdcp->write_sdu(rnti, srsran::srb_to_lcid(srb), std::move(pdu));
}

int rrc_nr::ue::pack_secondary_cell_group_rlc_cfg(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  // RLC for DRB1 (with fixed LCID)
  cell_group_cfg_pack.rlc_bearer_to_add_mod_list_present = true;
  cell_group_cfg_pack.rlc_bearer_to_add_mod_list.resize(1);
  auto& rlc_bearer                       = cell_group_cfg_pack.rlc_bearer_to_add_mod_list[0];
  rlc_bearer.lc_ch_id                    = drb1_lcid;
  rlc_bearer.served_radio_bearer_present = true;
  rlc_bearer.served_radio_bearer.set_drb_id();
  rlc_bearer.served_radio_bearer.drb_id() = 1;
  rlc_bearer.rlc_cfg_present              = true;
  rlc_bearer.rlc_cfg.set_um_bi_dir();
  rlc_bearer.rlc_cfg.um_bi_dir().ul_um_rlc.sn_field_len_present = true;
  rlc_bearer.rlc_cfg.um_bi_dir().ul_um_rlc.sn_field_len         = sn_field_len_um_opts::size12;
  rlc_bearer.rlc_cfg.um_bi_dir().dl_um_rlc.sn_field_len_present = true;
  rlc_bearer.rlc_cfg.um_bi_dir().dl_um_rlc.sn_field_len         = sn_field_len_um_opts::size12;
  rlc_bearer.rlc_cfg.um_bi_dir().dl_um_rlc.t_reassembly         = t_reassembly_opts::ms50;

  // MAC logical channel config
  rlc_bearer.mac_lc_ch_cfg_present                    = true;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params_present = true;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.prio    = 11;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.prioritised_bit_rate =
      asn1::rrc_nr::lc_ch_cfg_s::ul_specific_params_s_::prioritised_bit_rate_opts::kbps0;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.bucket_size_dur =
      asn1::rrc_nr::lc_ch_cfg_s::ul_specific_params_s_::bucket_size_dur_opts::ms100;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.lc_ch_group_present      = true;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.lc_ch_group              = 6;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.sched_request_id_present = true;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.sched_request_id         = 0;

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_secondary_cell_group_mac_cfg(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  // mac-CellGroup-Config for BSR and SR
  cell_group_cfg_pack.mac_cell_group_cfg_present                         = true;
  auto& mac_cell_group                                                   = cell_group_cfg_pack.mac_cell_group_cfg;
  mac_cell_group.sched_request_cfg_present                               = true;
  mac_cell_group.sched_request_cfg.sched_request_to_add_mod_list_present = true;
  mac_cell_group.sched_request_cfg.sched_request_to_add_mod_list.resize(1);
  mac_cell_group.sched_request_cfg.sched_request_to_add_mod_list[0].sched_request_id = 0;
  mac_cell_group.sched_request_cfg.sched_request_to_add_mod_list[0].sr_trans_max =
      asn1::rrc_nr::sched_request_to_add_mod_s::sr_trans_max_opts::n64;
  mac_cell_group.bsr_cfg_present            = true;
  mac_cell_group.bsr_cfg.periodic_bsr_timer = asn1::rrc_nr::bsr_cfg_s::periodic_bsr_timer_opts::sf20;
  mac_cell_group.bsr_cfg.retx_bsr_timer     = asn1::rrc_nr::bsr_cfg_s::retx_bsr_timer_opts::sf320;

  // Skip TAG and PHR config
  mac_cell_group.tag_cfg_present                     = false;
  mac_cell_group.tag_cfg.tag_to_add_mod_list_present = true;
  mac_cell_group.tag_cfg.tag_to_add_mod_list.resize(1);
  mac_cell_group.tag_cfg.tag_to_add_mod_list[0].tag_id           = 0;
  mac_cell_group.tag_cfg.tag_to_add_mod_list[0].time_align_timer = time_align_timer_opts::infinity;

  mac_cell_group.phr_cfg_present = false;
  mac_cell_group.phr_cfg.set_setup();
  mac_cell_group.phr_cfg.setup().phr_periodic_timer       = asn1::rrc_nr::phr_cfg_s::phr_periodic_timer_opts::sf500;
  mac_cell_group.phr_cfg.setup().phr_prohibit_timer       = asn1::rrc_nr::phr_cfg_s::phr_prohibit_timer_opts::sf200;
  mac_cell_group.phr_cfg.setup().phr_tx_pwr_factor_change = asn1::rrc_nr::phr_cfg_s::phr_tx_pwr_factor_change_opts::db3;
  mac_cell_group.phr_cfg.setup().multiple_phr             = true;
  mac_cell_group.phr_cfg.setup().dummy                    = false;
  mac_cell_group.phr_cfg.setup().phr_type2_other_cell     = false;
  mac_cell_group.phr_cfg.setup().phr_mode_other_cg        = asn1::rrc_nr::phr_cfg_s::phr_mode_other_cg_opts::real;

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_sp_cell_cfg_ded_init_dl_bwp(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.init_dl_bwp_present = true;

  pack_sp_cell_cfg_ded_init_dl_bwp_pdsch_cfg(cell_group_cfg_pack);
  pack_sp_cell_cfg_ded_init_dl_bwp_radio_link_monitoring(cell_group_cfg_pack);

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_sp_cell_cfg_ded_init_dl_bwp_radio_link_monitoring(
    asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.init_dl_bwp.radio_link_monitoring_cfg_present = true;
  auto& radio_link_monitoring = cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.init_dl_bwp.radio_link_monitoring_cfg;
  radio_link_monitoring.set_setup().fail_detection_res_to_add_mod_list_present = true;

  // add resource to detect RLF
  radio_link_monitoring.set_setup().fail_detection_res_to_add_mod_list.resize(1);
  auto& fail_detec_res_elem = radio_link_monitoring.set_setup().fail_detection_res_to_add_mod_list[0];
  fail_detec_res_elem.radio_link_monitoring_rs_id = 0;
  fail_detec_res_elem.purpose                     = asn1::rrc_nr::radio_link_monitoring_rs_s::purpose_opts::rlf;
  fail_detec_res_elem.detection_res.set_ssb_idx() = 0;

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_sp_cell_cfg_ded_init_dl_bwp_pdsch_cfg(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.init_dl_bwp.pdsch_cfg_present = true;
  auto& pdsch_cfg_dedicated = cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.init_dl_bwp.pdsch_cfg;

  pdsch_cfg_dedicated.set_setup();
  pdsch_cfg_dedicated.setup().dmrs_dl_for_pdsch_map_type_a_present = true;
  pdsch_cfg_dedicated.setup().dmrs_dl_for_pdsch_map_type_a.set_setup();
  pdsch_cfg_dedicated.setup().dmrs_dl_for_pdsch_map_type_a.setup().dmrs_add_position_present = true;
  pdsch_cfg_dedicated.setup().dmrs_dl_for_pdsch_map_type_a.setup().dmrs_add_position =
      asn1::rrc_nr::dmrs_dl_cfg_s::dmrs_add_position_opts::pos1;
  pdsch_cfg_dedicated.setup().tci_states_to_add_mod_list_present = true;
  pdsch_cfg_dedicated.setup().tci_states_to_add_mod_list.resize(1);
  pdsch_cfg_dedicated.setup().tci_states_to_add_mod_list[0].tci_state_id = 0;
  pdsch_cfg_dedicated.setup().tci_states_to_add_mod_list[0].qcl_type1.ref_sig.set_ssb();
  pdsch_cfg_dedicated.setup().tci_states_to_add_mod_list[0].qcl_type1.ref_sig.ssb() = 0;
  pdsch_cfg_dedicated.setup().tci_states_to_add_mod_list[0].qcl_type1.qcl_type =
      asn1::rrc_nr::qcl_info_s::qcl_type_opts::type_d;
  pdsch_cfg_dedicated.setup().res_alloc = pdsch_cfg_s::res_alloc_opts::res_alloc_type1;
  pdsch_cfg_dedicated.setup().rbg_size  = asn1::rrc_nr::pdsch_cfg_s::rbg_size_opts::cfg1;
  pdsch_cfg_dedicated.setup().prb_bundling_type.set_static_bundling();
  pdsch_cfg_dedicated.setup().prb_bundling_type.static_bundling().bundle_size_present = true;
  pdsch_cfg_dedicated.setup().prb_bundling_type.static_bundling().bundle_size =
      asn1::rrc_nr::pdsch_cfg_s::prb_bundling_type_c_::static_bundling_s_::bundle_size_opts::wideband;

  // ZP-CSI
  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list_present = false;
  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list.resize(1);
  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list[0].zp_csi_rs_res_id = 0;
  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list[0].res_map.freq_domain_alloc.set_row4();
  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list[0].res_map.freq_domain_alloc.row4().from_number(0b100);
  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list[0].res_map.nrof_ports =
      asn1::rrc_nr::csi_rs_res_map_s::nrof_ports_opts::p4;

  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list[0].res_map.first_ofdm_symbol_in_time_domain = 8;
  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list[0].res_map.cdm_type =
      asn1::rrc_nr::csi_rs_res_map_s::cdm_type_opts::fd_cdm2;
  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list[0].res_map.density.set_one();

  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list[0].res_map.freq_band.start_rb     = 0;
  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list[0].res_map.freq_band.nrof_rbs     = 52;
  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list[0].periodicity_and_offset_present = true;
  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list[0].periodicity_and_offset.set_slots80();
  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list[0].periodicity_and_offset.slots80() = 1;
  pdsch_cfg_dedicated.setup().p_zp_csi_rs_res_set_present                                       = false;
  pdsch_cfg_dedicated.setup().p_zp_csi_rs_res_set.set_setup();
  pdsch_cfg_dedicated.setup().p_zp_csi_rs_res_set.setup().zp_csi_rs_res_set_id = 0;
  pdsch_cfg_dedicated.setup().p_zp_csi_rs_res_set.setup().zp_csi_rs_res_id_list.resize(1);

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_sp_cell_cfg_ded_ul_cfg_init_ul_bwp_pucch_cfg(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  // PUCCH
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.ul_cfg.init_ul_bwp.pucch_cfg_present = true;
  auto& pucch_cfg = cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.ul_cfg.init_ul_bwp.pucch_cfg;

  pucch_cfg.set_setup();
  pucch_cfg.setup().format2_present = true;
  pucch_cfg.setup().format2.set_setup();
  pucch_cfg.setup().format2.setup().max_code_rate_present = true;
  pucch_cfg.setup().format2.setup().max_code_rate         = pucch_max_code_rate_opts::zero_dot25;

  // SR resources
  pucch_cfg.setup().sched_request_res_to_add_mod_list_present = true;
  pucch_cfg.setup().sched_request_res_to_add_mod_list.resize(1);
  auto& sr_res1                             = pucch_cfg.setup().sched_request_res_to_add_mod_list[0];
  sr_res1.sched_request_res_id              = 1;
  sr_res1.sched_request_id                  = 0;
  sr_res1.periodicity_and_offset_present    = true;
  sr_res1.periodicity_and_offset.set_sl40() = 8;
  sr_res1.res_present                       = true;
  sr_res1.res                               = 2; // PUCCH resource for SR

  // DL data
  pucch_cfg.setup().dl_data_to_ul_ack_present = true;

  if (parent->cfg.cell_list[0].duplex_mode == SRSRAN_DUPLEX_MODE_FDD) {
    pucch_cfg.setup().dl_data_to_ul_ack.resize(1);
    pucch_cfg.setup().dl_data_to_ul_ack[0] = 4;
  } else {
    pucch_cfg.setup().dl_data_to_ul_ack.resize(6);
    pucch_cfg.setup().dl_data_to_ul_ack[0] = 6;
    pucch_cfg.setup().dl_data_to_ul_ack[1] = 5;
    pucch_cfg.setup().dl_data_to_ul_ack[2] = 4;
    pucch_cfg.setup().dl_data_to_ul_ack[3] = 4;
    pucch_cfg.setup().dl_data_to_ul_ack[4] = 4;
    pucch_cfg.setup().dl_data_to_ul_ack[5] = 4;
  }

  // PUCCH Resource for format 1
  srsran_pucch_nr_resource_t resource_small = {};
  resource_small.starting_prb               = 0;
  resource_small.format                     = SRSRAN_PUCCH_NR_FORMAT_1;
  resource_small.initial_cyclic_shift       = 0;
  resource_small.nof_symbols                = 14;
  resource_small.start_symbol_idx           = 0;
  resource_small.time_domain_occ            = 0;

  // PUCCH Resource for format 2
  srsran_pucch_nr_resource_t resource_big = {};
  resource_big.starting_prb               = 51;
  resource_big.format                     = SRSRAN_PUCCH_NR_FORMAT_2;
  resource_big.nof_prb                    = 1;
  resource_big.nof_symbols                = 2;
  resource_big.start_symbol_idx           = 12;

  // Resource for SR
  srsran_pucch_nr_resource_t resource_sr = {};
  resource_sr.starting_prb               = 51;
  resource_sr.format                     = SRSRAN_PUCCH_NR_FORMAT_1;
  resource_sr.initial_cyclic_shift       = 0;
  resource_sr.nof_symbols                = 14;
  resource_sr.start_symbol_idx           = 0;
  resource_sr.time_domain_occ            = 0;

  // Make 3 possible resources
  pucch_cfg.setup().res_to_add_mod_list_present = true;
  pucch_cfg.setup().res_to_add_mod_list.resize(3);
  if (not srsran::make_phy_res_config(resource_small, pucch_cfg.setup().res_to_add_mod_list[0], 0)) {
    parent->logger.warning("Failed to create 1-2 bit NR PUCCH resource");
  }
  if (not srsran::make_phy_res_config(resource_big, pucch_cfg.setup().res_to_add_mod_list[1], 1)) {
    parent->logger.warning("Failed to create >2 bit NR PUCCH resource");
  }
  if (not srsran::make_phy_res_config(resource_sr, pucch_cfg.setup().res_to_add_mod_list[2], 2)) {
    parent->logger.warning("Failed to create SR NR PUCCH resource");
  }

  // Make 2 PUCCH resource sets
  pucch_cfg.setup().res_set_to_add_mod_list_present = true;
  pucch_cfg.setup().res_set_to_add_mod_list.resize(2);

  // Make PUCCH resource set for 1-2 bit
  pucch_cfg.setup().res_set_to_add_mod_list[0].pucch_res_set_id = 0;
  pucch_cfg.setup().res_set_to_add_mod_list[0].res_list.resize(8);
  for (auto& e : pucch_cfg.setup().res_set_to_add_mod_list[0].res_list) {
    e = 0;
  }

  // Make PUCCH resource set for >2 bit
  pucch_cfg.setup().res_set_to_add_mod_list[1].pucch_res_set_id = 1;
  pucch_cfg.setup().res_set_to_add_mod_list[1].res_list.resize(8);
  for (auto& e : pucch_cfg.setup().res_set_to_add_mod_list[1].res_list) {
    e = 1;
  }

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_sp_cell_cfg_ded_ul_cfg_init_ul_bwp_pusch_cfg(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  // PUSCH config
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.ul_cfg.init_ul_bwp.pusch_cfg_present = true;
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.ul_cfg.init_ul_bwp.pusch_cfg.set_setup();
  auto& pusch_cfg_ded = cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.ul_cfg.init_ul_bwp.pusch_cfg.setup();

  pusch_cfg_ded.dmrs_ul_for_pusch_map_type_a_present = true;
  pusch_cfg_ded.dmrs_ul_for_pusch_map_type_a.set_setup();
  pusch_cfg_ded.dmrs_ul_for_pusch_map_type_a.setup().dmrs_add_position_present = true;
  pusch_cfg_ded.dmrs_ul_for_pusch_map_type_a.setup().dmrs_add_position = dmrs_ul_cfg_s::dmrs_add_position_opts::pos1;
  // PUSH power control skipped
  pusch_cfg_ded.res_alloc = pusch_cfg_s::res_alloc_opts::res_alloc_type1;

  // UCI
  pusch_cfg_ded.uci_on_pusch_present = true;
  pusch_cfg_ded.uci_on_pusch.set_setup();
  pusch_cfg_ded.uci_on_pusch.setup().beta_offsets_present = true;
  pusch_cfg_ded.uci_on_pusch.setup().beta_offsets.set_semi_static();
  auto& beta_offset_semi_static                        = pusch_cfg_ded.uci_on_pusch.setup().beta_offsets.semi_static();
  beta_offset_semi_static.beta_offset_ack_idx1_present = true;
  beta_offset_semi_static.beta_offset_ack_idx1         = 9;
  beta_offset_semi_static.beta_offset_ack_idx2_present = true;
  beta_offset_semi_static.beta_offset_ack_idx2         = 9;
  beta_offset_semi_static.beta_offset_ack_idx3_present = true;
  beta_offset_semi_static.beta_offset_ack_idx3         = 9;
  beta_offset_semi_static.beta_offset_csi_part1_idx1_present = true;
  beta_offset_semi_static.beta_offset_csi_part1_idx1         = 6;
  beta_offset_semi_static.beta_offset_csi_part1_idx2_present = true;
  beta_offset_semi_static.beta_offset_csi_part1_idx2         = 6;
  beta_offset_semi_static.beta_offset_csi_part2_idx1_present = true;
  beta_offset_semi_static.beta_offset_csi_part2_idx1         = 6;
  beta_offset_semi_static.beta_offset_csi_part2_idx2_present = true;
  beta_offset_semi_static.beta_offset_csi_part2_idx2         = 6;
  pusch_cfg_ded.uci_on_pusch.setup().scaling                 = uci_on_pusch_s::scaling_opts::f1;

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_sp_cell_cfg_ded_ul_cfg_init_ul_bwp(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.ul_cfg.init_ul_bwp_present = true;

  pack_sp_cell_cfg_ded_ul_cfg_init_ul_bwp_pucch_cfg(cell_group_cfg_pack);
  pack_sp_cell_cfg_ded_ul_cfg_init_ul_bwp_pusch_cfg(cell_group_cfg_pack);

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_sp_cell_cfg_ded_ul_cfg(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  // UL config dedicated
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.ul_cfg_present = true;

  pack_sp_cell_cfg_ded_ul_cfg_init_ul_bwp(cell_group_cfg_pack);

  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.ul_cfg.first_active_ul_bwp_id_present = true;
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.ul_cfg.first_active_ul_bwp_id         = 0;

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_sp_cell_cfg_ded_pdcch_serving_cell_cfg(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.pdcch_serving_cell_cfg_present = true;
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.pdcch_serving_cell_cfg.set_setup();

  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.pdsch_serving_cell_cfg_present = true;
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.pdsch_serving_cell_cfg.set_setup();
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.pdsch_serving_cell_cfg.setup().nrof_harq_processes_for_pdsch_present =
      true;
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.pdsch_serving_cell_cfg.setup().nrof_harq_processes_for_pdsch =
      pdsch_serving_cell_cfg_s::nrof_harq_processes_for_pdsch_opts::n16;

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_sp_cell_cfg_ded(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  // SP Cell Dedicated config
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded_present                        = true;
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.first_active_dl_bwp_id_present = true;

  if (parent->cfg.cell_list[0].duplex_mode == SRSRAN_DUPLEX_MODE_FDD) {
    cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.first_active_dl_bwp_id = 0;
  } else {
    cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.first_active_dl_bwp_id = 1;
  }

  pack_sp_cell_cfg_ded_ul_cfg(cell_group_cfg_pack);
  pack_sp_cell_cfg_ded_init_dl_bwp(cell_group_cfg_pack);

  // Serving cell config (only to setup)
  pack_sp_cell_cfg_ded_pdcch_serving_cell_cfg(cell_group_cfg_pack);

  // spCellConfig
  if (fill_sp_cell_cfg_from_enb_cfg(parent->cfg, UE_PSCELL_CC_IDX, cell_group_cfg_pack.sp_cell_cfg) != SRSRAN_SUCCESS) {
    parent->logger.error("Failed to pack spCellConfig for rnti=0x%x", rnti);
  }

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_recfg_with_sync_sp_cell_cfg_common_dl_cfg_common_phy_cell_group_cfg(
    asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  cell_group_cfg_pack.phys_cell_group_cfg_present = true;
  cell_group_cfg_pack.phys_cell_group_cfg.pdsch_harq_ack_codebook =
      phys_cell_group_cfg_s::pdsch_harq_ack_codebook_opts::dynamic_value;

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_recfg_with_sync_sp_cell_cfg_common_dl_cfg_init_dl_bwp_pdsch_cfg_common(
    asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  // PDSCH config common
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.dl_cfg_common.init_dl_bwp
      .pdsch_cfg_common_present = true;
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.dl_cfg_common.init_dl_bwp.pdsch_cfg_common
      .set_setup();

  auto& pdsch_cfg_common = cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.dl_cfg_common.init_dl_bwp
                               .pdsch_cfg_common.setup();
  pdsch_cfg_common.pdsch_time_domain_alloc_list_present = true;
  pdsch_cfg_common.pdsch_time_domain_alloc_list.resize(1);
  pdsch_cfg_common.pdsch_time_domain_alloc_list[0].map_type = pdsch_time_domain_res_alloc_s::map_type_opts::type_a;
  pdsch_cfg_common.pdsch_time_domain_alloc_list[0].start_symbol_and_len = 40;

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_recfg_with_sync_sp_cell_cfg_common_dl_cfg_init_dl_bwp(
    asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.dl_cfg_common.init_dl_bwp_present = true;
  auto& init_dl_bwp = cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.dl_cfg_common.init_dl_bwp;

  init_dl_bwp.generic_params.location_and_bw    = 14025;
  init_dl_bwp.generic_params.subcarrier_spacing = subcarrier_spacing_opts::khz15;

  pack_recfg_with_sync_sp_cell_cfg_common_dl_cfg_init_dl_bwp_pdsch_cfg_common(cell_group_cfg_pack);

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_recfg_with_sync_sp_cell_cfg_common_dl_cfg_common(
    asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  // DL config
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.dl_cfg_common_present = true;

  pack_recfg_with_sync_sp_cell_cfg_common_dl_cfg_common_phy_cell_group_cfg(cell_group_cfg_pack);
  pack_recfg_with_sync_sp_cell_cfg_common_dl_cfg_init_dl_bwp(cell_group_cfg_pack);

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_recfg_with_sync_sp_cell_cfg_common_ul_cfg_common_init_ul_bwp_pusch_cfg_common(
    asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  // PUSCH config common
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.ul_cfg_common.init_ul_bwp
      .pusch_cfg_common_present = true;
  auto& pusch_cfg_common_pack =
      cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.ul_cfg_common.init_ul_bwp.pusch_cfg_common;
  pusch_cfg_common_pack.set_setup();
  pusch_cfg_common_pack.setup().pusch_time_domain_alloc_list_present = true;
  pusch_cfg_common_pack.setup().pusch_time_domain_alloc_list.resize(2);
  pusch_cfg_common_pack.setup().pusch_time_domain_alloc_list[0].k2_present = true;
  pusch_cfg_common_pack.setup().pusch_time_domain_alloc_list[0].k2         = 4;
  pusch_cfg_common_pack.setup().pusch_time_domain_alloc_list[0].map_type =
      asn1::rrc_nr::pusch_time_domain_res_alloc_s::map_type_opts::type_a;
  pusch_cfg_common_pack.setup().pusch_time_domain_alloc_list[0].start_symbol_and_len = 27;
  pusch_cfg_common_pack.setup().pusch_time_domain_alloc_list[1].k2_present           = true;
  pusch_cfg_common_pack.setup().pusch_time_domain_alloc_list[1].k2                   = 3;
  pusch_cfg_common_pack.setup().pusch_time_domain_alloc_list[1].map_type =
      asn1::rrc_nr::pusch_time_domain_res_alloc_s::map_type_opts::type_a;
  pusch_cfg_common_pack.setup().pusch_time_domain_alloc_list[1].start_symbol_and_len = 27;
  pusch_cfg_common_pack.setup().p0_nominal_with_grant_present                        = true;
  pusch_cfg_common_pack.setup().p0_nominal_with_grant                                = -60;

  // PUCCH config common
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.ul_cfg_common.init_ul_bwp
      .pucch_cfg_common_present = true;
  auto& pucch_cfg_common_pack =
      cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.ul_cfg_common.init_ul_bwp.pucch_cfg_common;
  pucch_cfg_common_pack.set_setup();
  pucch_cfg_common_pack.setup().pucch_group_hop    = asn1::rrc_nr::pucch_cfg_common_s::pucch_group_hop_opts::neither;
  pucch_cfg_common_pack.setup().p0_nominal_present = true;
  pucch_cfg_common_pack.setup().p0_nominal         = -60;

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_recfg_with_sync_sp_cell_cfg_common_ul_cfg_common_init_ul_bwp(
    asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.ul_cfg_common.init_ul_bwp_present = true;
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.ul_cfg_common.init_ul_bwp.generic_params
      .location_and_bw = 14025;
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.ul_cfg_common.init_ul_bwp.generic_params
      .subcarrier_spacing = subcarrier_spacing_opts::khz15;

  pack_recfg_with_sync_sp_cell_cfg_common_ul_cfg_common_init_ul_bwp_pusch_cfg_common(cell_group_cfg_pack);

  return SRSRAN_ERROR;
}

int rrc_nr::ue::pack_recfg_with_sync_sp_cell_cfg_common_ul_cfg_common(
    asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  // UL config
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.ul_cfg_common_present = true;
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.ul_cfg_common.dummy = time_align_timer_opts::ms500;

  pack_recfg_with_sync_sp_cell_cfg_common_ul_cfg_common_init_ul_bwp(cell_group_cfg_pack);

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_recfg_with_sync_sp_cell_cfg_common(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  auto& pscell_cfg = parent->cfg.cell_list.at(UE_PSCELL_CC_IDX);

  if (pscell_cfg.duplex_mode == SRSRAN_DUPLEX_MODE_TDD) {
    cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.smtc.release();
  }

  // DL config
  pack_recfg_with_sync_sp_cell_cfg_common_dl_cfg_common(cell_group_cfg_pack);

  // UL config
  pack_recfg_with_sync_sp_cell_cfg_common_ul_cfg_common(cell_group_cfg_pack);

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_recfg_with_sync(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  // Reconfig with Sync
  cell_group_cfg_pack.cell_group_id = 1; // 0 identifies the MCG. Other values identify SCGs.

  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync_present   = true;
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.new_ue_id = rnti;
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.t304      = recfg_with_sync_s::t304_opts::ms1000;

  pack_recfg_with_sync_sp_cell_cfg_common(cell_group_cfg_pack);

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_secondary_cell_group_sp_cell_cfg(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  cell_group_cfg_pack.sp_cell_cfg_present               = true;
  cell_group_cfg_pack.sp_cell_cfg.serv_cell_idx_present = true;
  cell_group_cfg_pack.sp_cell_cfg.serv_cell_idx = 1; // Serving cell ID of a PSCell. The PCell of the MCG uses ID 0.

  pack_sp_cell_cfg_ded(cell_group_cfg_pack);
  pack_recfg_with_sync(cell_group_cfg_pack);

  return SRSRAN_SUCCESS;
}

// Helper for the RRC Reconfiguration sender to pack hard-coded config
int rrc_nr::ue::pack_secondary_cell_group_cfg(asn1::dyn_octstring& packed_secondary_cell_config)
{
  auto& cell_group_cfg_pack = cell_group_cfg;

  pack_secondary_cell_group_rlc_cfg(cell_group_cfg_pack);
  pack_secondary_cell_group_mac_cfg(cell_group_cfg_pack);
  pack_secondary_cell_group_sp_cell_cfg(cell_group_cfg_pack);

  // make sufficiant space
  packed_secondary_cell_config.resize(256);
  asn1::bit_ref bref_pack(packed_secondary_cell_config.data(), packed_secondary_cell_config.size());
  if (cell_group_cfg_pack.pack(bref_pack) != asn1::SRSASN_SUCCESS) {
    parent->logger.error("Failed to pack NR secondary cell config");
    return SRSRAN_ERROR;
  }
  packed_secondary_cell_config.resize(bref_pack.distance_bytes());

  log_rrc_container(Tx, packed_secondary_cell_config, cell_group_cfg_pack, "nr-SecondaryCellGroupConfig-r15");

  return SRSRAN_SUCCESS;
}

// Packs a hard-coded RRC Reconfiguration with fixed params for all layers (for now)
int rrc_nr::ue::pack_rrc_reconfiguration(asn1::dyn_octstring& packed_rrc_reconfig)
{
  rrc_recfg_s reconfig;
  reconfig.rrc_transaction_id = ((transaction_id++) % 4u);
  rrc_recfg_ies_s& recfg_ies  = reconfig.crit_exts.set_rrc_recfg();

  // add secondary cell group config
  recfg_ies.secondary_cell_group_present = true;

  if (pack_secondary_cell_group_cfg(recfg_ies.secondary_cell_group) == SRSRAN_ERROR) {
    parent->logger.error("Failed to pack secondary cell group");
    return SRSRAN_ERROR;
  }

  // now pack ..
  packed_rrc_reconfig.resize(512);
  asn1::bit_ref bref_pack(packed_rrc_reconfig.data(), packed_rrc_reconfig.size());
  if (reconfig.pack(bref_pack) != asn1::SRSASN_SUCCESS) {
    parent->logger.error("Failed to pack RRC Reconfiguration");
    return SRSRAN_ERROR;
  }
  packed_rrc_reconfig.resize(bref_pack.distance_bytes());

  return SRSRAN_SUCCESS;
}

// Packs a hard-coded NR radio bearer config with fixed params for RLC/PDCP (for now)
int rrc_nr::ue::pack_nr_radio_bearer_config(asn1::dyn_octstring& packed_nr_bearer_config)
{
  // set security config
  auto& radio_bearer_cfg_pack                        = radio_bearer_cfg;
  radio_bearer_cfg_pack.security_cfg_present         = true;
  auto& sec_cfg                                      = radio_bearer_cfg_pack.security_cfg;
  sec_cfg.key_to_use_present                         = true;
  sec_cfg.key_to_use                                 = asn1::rrc_nr::security_cfg_s::key_to_use_opts::secondary;
  sec_cfg.security_algorithm_cfg_present             = true;
  sec_cfg.security_algorithm_cfg.ciphering_algorithm = ciphering_algorithm_opts::nea0;
  sec_cfg.security_algorithm_cfg.integrity_prot_algorithm_present = true;
  sec_cfg.security_algorithm_cfg.integrity_prot_algorithm         = integrity_prot_algorithm_opts::nia0;

  // pack it
  packed_nr_bearer_config.resize(128);
  asn1::bit_ref bref_pack(packed_nr_bearer_config.data(), packed_nr_bearer_config.size());
  if (radio_bearer_cfg_pack.pack(bref_pack) != asn1::SRSASN_SUCCESS) {
    parent->logger.error("Failed to pack NR radio bearer config");
    return SRSRAN_ERROR;
  }

  // resize to packed length
  packed_nr_bearer_config.resize(bref_pack.distance_bytes());

  log_rrc_container(Tx, packed_nr_bearer_config, radio_bearer_cfg_pack, "nr-RadioBearerConfig1-r15");

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::handle_sgnb_addition_request(uint16_t eutra_rnti_, const sgnb_addition_req_params_t& req_params)
{
  // Add DRB1 to RLC and PDCP
  if (add_drb() != SRSRAN_SUCCESS) {
    parent->logger.error("Failed to configure DRB");
    parent->rrc_eutra->sgnb_addition_reject(eutra_rnti_);
    return SRSRAN_ERROR;
  }

  // provide hard-coded NR configs
  rrc_eutra_interface_rrc_nr::sgnb_addition_ack_params_t ack_params = {};
  if (pack_rrc_reconfiguration(ack_params.nr_secondary_cell_group_cfg_r15) == SRSRAN_ERROR) {
    parent->logger.error("Failed to pack RRC Reconfiguration. Sending SgNB addition reject.");
    parent->rrc_eutra->sgnb_addition_reject(eutra_rnti_);
    return SRSRAN_ERROR;
  }

  if (pack_nr_radio_bearer_config(ack_params.nr_radio_bearer_cfg1_r15) == SRSRAN_ERROR) {
    parent->logger.error("Failed to pack NR radio bearer config. Sending SgNB addition reject.");
    parent->rrc_eutra->sgnb_addition_reject(eutra_rnti_);
    return SRSRAN_ERROR;
  }

  // send response to EUTRA
  ack_params.nr_rnti       = rnti;
  ack_params.eps_bearer_id = req_params.eps_bearer_id;
  parent->rrc_eutra->sgnb_addition_ack(eutra_rnti_, ack_params);

  // recognize RNTI as ENDC user
  endc       = true;
  eutra_rnti = eutra_rnti_;

  return SRSRAN_SUCCESS;
}

void rrc_nr::ue::crnti_ce_received()
{
  // Assume NSA mode active
  if (endc) {
    // send SgNB addition complete for ENDC users
    parent->rrc_eutra->sgnb_addition_complete(eutra_rnti, rnti);

    // stop RX MSG3/MSG5 activity timer on MAC CE RNTI reception
    set_activity_timeout(UE_INACTIVITY_TIMEOUT);
    parent->logger.debug("Received MAC CE-RNTI for 0x%x - stopping MSG3/MSG5 timer, starting inactivity timer", rnti);

    // Add DRB1 to MAC
    for (auto& drb : cell_group_cfg.rlc_bearer_to_add_mod_list) {
      uecfg.ue_bearers[drb.lc_ch_id].direction = mac_lc_ch_cfg_t::BOTH;
      uecfg.ue_bearers[drb.lc_ch_id].group     = drb.mac_lc_ch_cfg.ul_specific_params.lc_ch_group;
    }

    // Update UE phy params
    srsran::make_pdsch_cfg_from_serv_cell(cell_group_cfg.sp_cell_cfg.sp_cell_cfg_ded, &uecfg.phy_cfg.pdsch);
    srsran::make_csi_cfg_from_serv_cell(cell_group_cfg.sp_cell_cfg.sp_cell_cfg_ded, &uecfg.phy_cfg.csi);
    srsran::make_phy_ssb_cfg(parent->cfg.cell_list[0].phy_cell.carrier,
                             cell_group_cfg.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common,
                             &uecfg.phy_cfg.ssb);
    srsran::make_duplex_cfg_from_serv_cell(cell_group_cfg.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common,
                                           &uecfg.phy_cfg.duplex);

    parent->mac->ue_cfg(rnti, uecfg);
  }
}

/**
 * @brief Set DRB configuration
 *
 * The function sets and configures all relavant fields for the DRB configuration (MAC, RLC, PDCP) in the
 * cellGroupConfig and also adds the bearer to the local RLC and PDCP entities.
 *
 * @return int SRSRAN_SUCCESS on success
 */
int rrc_nr::ue::add_drb()
{
  // RLC for DRB1 (with fixed LCID) inside cell_group_cfg
  auto& cell_group_cfg_pack = cell_group_cfg;

  cell_group_cfg_pack.rlc_bearer_to_add_mod_list_present = true;
  cell_group_cfg_pack.rlc_bearer_to_add_mod_list.resize(1);
  auto& rlc_bearer                       = cell_group_cfg_pack.rlc_bearer_to_add_mod_list[0];
  rlc_bearer.lc_ch_id                    = drb1_lcid;
  rlc_bearer.served_radio_bearer_present = true;
  rlc_bearer.served_radio_bearer.set_drb_id();
  rlc_bearer.served_radio_bearer.drb_id() = 1;
  rlc_bearer.rlc_cfg_present              = true;
  rlc_bearer.rlc_cfg.set_um_bi_dir();
  rlc_bearer.rlc_cfg.um_bi_dir().ul_um_rlc.sn_field_len_present = true;
  rlc_bearer.rlc_cfg.um_bi_dir().ul_um_rlc.sn_field_len         = sn_field_len_um_opts::size12;
  rlc_bearer.rlc_cfg.um_bi_dir().dl_um_rlc.sn_field_len_present = true;
  rlc_bearer.rlc_cfg.um_bi_dir().dl_um_rlc.sn_field_len         = sn_field_len_um_opts::size12;
  rlc_bearer.rlc_cfg.um_bi_dir().dl_um_rlc.t_reassembly         = t_reassembly_opts::ms50;

  // add RLC bearer
  srsran::rlc_config_t rlc_cfg;
  /// NOTE, we need to pass the radio-bearer to the rlc_config
  if (srsran::make_rlc_config_t(cell_group_cfg.rlc_bearer_to_add_mod_list[0].rlc_cfg,
                                rlc_bearer.served_radio_bearer.drb_id(),
                                &rlc_cfg) != SRSRAN_SUCCESS) {
    parent->logger.error("Failed to build RLC config");
    return SRSRAN_ERROR;
  }
  parent->rlc->add_bearer(rnti, drb1_lcid, rlc_cfg);

  // MAC logical channel config
  rlc_bearer.mac_lc_ch_cfg_present                    = true;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params_present = true;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.prio    = 11;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.prioritised_bit_rate =
      asn1::rrc_nr::lc_ch_cfg_s::ul_specific_params_s_::prioritised_bit_rate_opts::kbps0;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.bucket_size_dur =
      asn1::rrc_nr::lc_ch_cfg_s::ul_specific_params_s_::bucket_size_dur_opts::ms100;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.lc_ch_group_present      = true;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.lc_ch_group              = 3;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.sched_request_id_present = true;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.sched_request_id         = 0;
  // TODO: add LC config to MAC

  // PDCP config goes into radio_bearer_cfg
  auto& radio_bearer_cfg_pack                       = radio_bearer_cfg;
  radio_bearer_cfg_pack.drb_to_add_mod_list_present = true;
  radio_bearer_cfg_pack.drb_to_add_mod_list.resize(1);

  // configure fixed DRB1
  auto& drb_item                                = radio_bearer_cfg_pack.drb_to_add_mod_list[0];
  drb_item.drb_id                               = 1;
  drb_item.cn_assoc_present                     = true;
  drb_item.cn_assoc.set_eps_bearer_id()         = 5;
  drb_item.pdcp_cfg_present                     = true;
  drb_item.pdcp_cfg.ciphering_disabled_present  = true;
  drb_item.pdcp_cfg.drb_present                 = true;
  drb_item.pdcp_cfg.drb.pdcp_sn_size_dl_present = true;
  drb_item.pdcp_cfg.drb.pdcp_sn_size_dl         = asn1::rrc_nr::pdcp_cfg_s::drb_s_::pdcp_sn_size_dl_opts::len18bits;
  drb_item.pdcp_cfg.drb.pdcp_sn_size_ul_present = true;
  drb_item.pdcp_cfg.drb.pdcp_sn_size_ul         = asn1::rrc_nr::pdcp_cfg_s::drb_s_::pdcp_sn_size_ul_opts::len18bits;
  drb_item.pdcp_cfg.drb.discard_timer_present   = true;
  drb_item.pdcp_cfg.drb.discard_timer           = asn1::rrc_nr::pdcp_cfg_s::drb_s_::discard_timer_opts::ms100;
  drb_item.pdcp_cfg.drb.hdr_compress.set_not_used();
  drb_item.pdcp_cfg.t_reordering_present = true;
  drb_item.pdcp_cfg.t_reordering         = asn1::rrc_nr::pdcp_cfg_s::t_reordering_opts::ms0;

  // Add DRB1 to PDCP
  srsran::pdcp_config_t pdcp_cnfg = srsran::make_drb_pdcp_config_t(drb_item.drb_id, false, drb_item.pdcp_cfg);
  parent->pdcp->add_bearer(rnti, rlc_bearer.lc_ch_id, pdcp_cnfg);

  // Note: DRB1 is only activated in the MAC when the C-RNTI CE is received

  return SRSRAN_SUCCESS;
}

void rrc_nr::ue::handle_rrc_setup_request(const asn1::rrc_nr::rrc_setup_request_s& msg)
{
  if (not parent->ngap->is_amf_connected()) {
    parent->logger.error("MME isn't connected. Sending Connection Reject");
    const uint8_t max_wait_time_secs = 16;
    send_rrc_reject(max_wait_time_secs); // See TS 38.331, RejectWaitTime
    return;
  }

  // TODO: Allocate PUCCH resources and reject if not available

  switch (msg.rrc_setup_request.ue_id.type().value) {
    case asn1::rrc_nr::init_ue_id_c::types_opts::ng_minus5_g_s_tmsi_part1:
      // TODO: communicate with NGAP
      break;
    case asn1::rrc_nr::init_ue_id_c::types_opts::random_value:
      // TODO: communicate with NGAP
      break;
    default:
      parent->logger.error("Unsupported RRCSetupRequest");
  }

  send_rrc_setup();
  set_activity_timeout(UE_INACTIVITY_TIMEOUT);
}

/// TS 38.331, RRCReject message
void rrc_nr::ue::send_rrc_reject(uint8_t reject_wait_time_secs)
{
  dl_ccch_msg_s     msg;
  rrc_reject_ies_s& reject = msg.msg.set_c1().set_rrc_reject().crit_exts.set_rrc_reject();
  if (reject_wait_time_secs > 0) {
    reject.wait_time_present = true;
    reject.wait_time         = reject_wait_time_secs;
  }
  send_dl_ccch(msg);
}

/// TS 38.331, RRCSetup
void rrc_nr::ue::send_rrc_setup()
{
  dl_ccch_msg_s msg;
  rrc_setup_s&  setup        = msg.msg.set_c1().set_rrc_setup();
  setup.rrc_transaction_id   = (uint8_t)((transaction_id++) % 4);
  rrc_setup_ies_s& setup_ies = setup.crit_exts.set_rrc_setup();

  // Fill RRC Setup
  // Note: See 5.3.5.6.3 - SRB addition/modification
  setup_ies.radio_bearer_cfg.srb_to_add_mod_list_present = true;
  setup_ies.radio_bearer_cfg.srb_to_add_mod_list.resize(1);
  srb_to_add_mod_s& srb1 = setup_ies.radio_bearer_cfg.srb_to_add_mod_list[0];
  srb1.srb_id            = 1;

  send_dl_ccch(msg);
}

/// TS 38.331, RRCSetupComplete
void rrc_nr::ue::handle_rrc_setup_complete(const asn1::rrc_nr::rrc_setup_complete_s& msg)
{
  // TODO: handle RRCSetupComplete

  send_security_mode_command();
}

/// TS 38.331, SecurityModeCommand
void rrc_nr::ue::send_security_mode_command()
{
  asn1::rrc_nr::dl_dcch_msg_s dl_dcch_msg;
  dl_dcch_msg.msg.set_c1().set_security_mode_cmd().rrc_transaction_id = (uint8_t)((transaction_id++) % 4);
  security_mode_cmd_ies_s& ies = dl_dcch_msg.msg.c1().security_mode_cmd().crit_exts.set_security_mode_cmd();

  ies.security_cfg_smc.security_algorithm_cfg.integrity_prot_algorithm_present = true;
  ies.security_cfg_smc.security_algorithm_cfg.integrity_prot_algorithm.value   = integrity_prot_algorithm_opts::nia0;
  ies.security_cfg_smc.security_algorithm_cfg.ciphering_algorithm.value        = ciphering_algorithm_opts::nea0;

  send_dl_dcch(srsran::nr_srb::srb1, dl_dcch_msg);
}

/// TS 38.331, SecurityModeComplete
void rrc_nr::ue::handle_security_mode_complete(const asn1::rrc_nr::security_mode_complete_s& msg)
{
  // TODO: handle SecurityModeComplete

  send_rrc_reconfiguration();
}

void rrc_nr::ue::send_rrc_reconfiguration()
{
  asn1::rrc_nr::dl_dcch_msg_s dl_dcch_msg;
  dl_dcch_msg.msg.set_c1().set_rrc_recfg().rrc_transaction_id = (uint8_t)((transaction_id++) % 4);
  rrc_recfg_ies_s& ies = dl_dcch_msg.msg.c1().rrc_recfg().crit_exts.set_rrc_recfg();

  ies.non_crit_ext_present                   = true;
  ies.non_crit_ext.master_cell_group_present = false; // TODO

  // Update lower layers

  send_dl_dcch(srsran::nr_srb::srb1, dl_dcch_msg);
}

void rrc_nr::ue::handle_rrc_reconfiguration_complete(const asn1::rrc_nr::rrc_recfg_complete_s& msg)
{
  // TODO: handle RRCReconfComplete
}

void rrc_nr::ue::handle_ul_information_transfer(const asn1::rrc_nr::ul_info_transfer_s& msg)
{
  // TODO: handle UL information transfer
}

/**
 * @brief Deactivate all Bearers (MAC logical channel) for this specific RNTI
 *
 * The function iterates over the bearers or MAC logical channels and deactivates them by setting each one to IDLE
 */
void rrc_nr::ue::deactivate_bearers()
{
  // Iterate over the bearers (MAC LC CH) and set each of them to IDLE
  for (auto& ue_bearer : uecfg.ue_bearers) {
    ue_bearer.direction = mac_lc_ch_cfg_t::IDLE;
  }

  // No need to check the returned value, as the function ue_cfg will return SRSRAN_SUCCESS (it asserts if it fails)
  parent->mac->ue_cfg(rnti, uecfg);
}

template <class M>
void rrc_nr::ue::log_rrc_message(srsran::nr_srb          srb,
                                 const direction_t       dir,
                                 srsran::const_byte_span pdu,
                                 const M&                msg,
                                 const char*             msg_type)
{
  fmt::memory_buffer strbuf;
  fmt::format_to(strbuf, "rnti=0x{:x}, {}", rnti, srsran::get_srb_name(srb));
  parent->log_rrc_message(srsran::to_c_str(strbuf), Tx, pdu, msg, msg_type);
}

template <class M>
void rrc_nr::ue::log_rrc_container(const direction_t       dir,
                                   srsran::const_byte_span pdu,
                                   const M&                msg,
                                   const char*             msg_type)
{
  fmt::memory_buffer strbuf;
  fmt::format_to(strbuf, "rnti=0x{:x}, container", rnti);
  parent->log_rrc_message(srsran::to_c_str(strbuf), Tx, pdu, msg, msg_type);
}

} // namespace srsenb