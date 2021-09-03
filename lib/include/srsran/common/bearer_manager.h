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

#ifndef SRSRAN_BEARER_MANAGER_H
#define SRSRAN_BEARER_MANAGER_H

#include "srsenb/hdr/common/common_enb.h"
#include "srsran/common/common.h"
#include "srsran/common/rwlock_guard.h"
#include "srsran/srslog/srslog.h"
#include <map>
#include <stdint.h>

namespace srsran {

namespace detail {

/**
 * @brief Implementation of UE bearer manager internal functionality that is common to both srsue and
 * srsenb applications
 */
class ue_bearer_manager_impl
{
public:
  struct radio_bearer_t {
    srsran::srsran_rat_t rat;
    uint32_t             lcid;
    uint32_t             eps_bearer_id;
    bool                 is_valid() const { return rat != srsran_rat_t::nulltype; }
  };
  static const radio_bearer_t invalid_rb;

  /// Registers EPS bearer with PDCP RAT type and LCID
  bool add_eps_bearer(uint8_t eps_bearer_id, srsran::srsran_rat_t rat, uint32_t lcid);

  /// Single EPS bearer is removed from map when the associated DRB is deleted
  bool remove_eps_bearer(uint8_t eps_bearer_id);

  void reset();

  bool has_active_radio_bearer(uint32_t eps_bearer_id);

  radio_bearer_t get_radio_bearer(uint32_t eps_bearer_id);

  radio_bearer_t get_lcid_bearer(uint32_t lcid);

private:
  using eps_rb_map_t = std::map<uint32_t, radio_bearer_t>;

  eps_rb_map_t                 bearers;
  std::map<uint32_t, uint32_t> lcid_to_eps_bearer_id;
};

} // namespace detail
} // namespace srsran

namespace srsue {

/**
 * @brief Helper class to manage the mapping between EPS bearer and radio bearer
 *
 * The class maps EPS bearers that are known to NAS and GW (UE)
 * to radio bearer (RB) that are only known to RRC.
 * Since the lifetime of a EPS bearer is usually longer than the lifetime of a RB,
 * the GW/GTPU needs to query the Stack to check whether a
 * given EPS bearer is active, i.e. a DRB is established, or not.
 *
 * The class also maps between RATs since each LCID can exist on either EUTRA or NR RATs, or both.
 *
 * Since the access of this class is happening from two different threads (GW+RRC/Stack)
 * it's public interface is protected.
 *
 * The class provides two interfaces to be used with RNTI or without. The version without
 * RNTI is used by the UE. The version with RNTI in the interface is intented to be
 * used by the eNB.
 *
 */
class ue_bearer_manager
{
public:
  using radio_bearer_t = srsran::detail::ue_bearer_manager_impl::radio_bearer_t;

  ue_bearer_manager();
  ue_bearer_manager(const ue_bearer_manager&) = delete;
  ue_bearer_manager& operator=(const ue_bearer_manager&) = delete;
  ~ue_bearer_manager();

  // RRC interface
  /// Registers EPS bearer with PDCP RAT type and LCID
  void add_eps_bearer(uint8_t eps_bearer_id, srsran::srsran_rat_t rat, uint32_t lcid);

  /// Single EPS bearer is removed from map when the associated DRB is deleted
  void remove_eps_bearer(uint8_t eps_bearer_id);

  /// All registered bearer are removed (e.g. after connection release)
  void reset();

  bool has_active_radio_bearer(uint32_t eps_bearer_id) { return impl.has_active_radio_bearer(eps_bearer_id); }

  radio_bearer_t get_radio_bearer(uint32_t eps_bearer_id) { return impl.get_radio_bearer(eps_bearer_id); }

  radio_bearer_t get_lcid_bearer(uint32_t lcid) { return impl.get_lcid_bearer(lcid); }

private:
  pthread_rwlock_t                       rwlock = {}; /// RW lock to protect access from RRC/GW threads
  srslog::basic_logger&                  logger;
  srsran::detail::ue_bearer_manager_impl impl;
};

} // namespace srsue

namespace srsenb {

class enb_bearer_manager
{
public:
  using radio_bearer_t = srsran::detail::ue_bearer_manager_impl::radio_bearer_t;

  enb_bearer_manager();

  /// Multi-user interface (see comments above)
  void           add_eps_bearer(uint16_t rnti, uint8_t eps_bearer_id, srsran::srsran_rat_t rat, uint32_t lcid);
  void           remove_eps_bearer(uint16_t rnti, uint8_t eps_bearer_id);
  void           rem_user(uint16_t rnti);
  bool           has_active_radio_bearer(uint16_t rnti, uint32_t eps_bearer_id);
  radio_bearer_t get_radio_bearer(uint16_t rnti, uint32_t eps_bearer_id);
  radio_bearer_t get_lcid_bearer(uint16_t rnti, uint32_t lcid);

private:
  srslog::basic_logger& logger;

  srsenb::rnti_map_t<srsran::detail::ue_bearer_manager_impl> users_map;
};

} // namespace srsenb

#endif // SRSRAN_BEARER_MANAGER_H