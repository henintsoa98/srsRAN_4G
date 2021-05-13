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
#ifndef SRSUE_INTRA_MEASURE_BASE_H
#define SRSUE_INTRA_MEASURE_BASE_H

#include <srsran/common/common.h>
#include <srsran/common/threads.h>
#include <srsran/common/tti_sync_cv.h>
#include <srsran/srsran.h>

#include "scell_recv.h"

namespace srsue {
namespace scell {

/**
 * @brief Describes a generic base class to perform intra-frequency measurements
 */
class intra_measure_base : public srsran::thread
{
  /*
   * The intra-cell measurment has 5 different states:
   *  - idle: it has been initiated and it is waiting to get configured to start capturing samples. From any state
   *          except quit can transition to idle.
   *  - wait: waits for at least intra_freq_meas_period_ms since last receive start and goes to receive.
   *  - receive: captures base-band samples for intra_freq_meas_len_ms and goes to measure.
   *  - measure: enables the inner thread to start the measuring function. The asynchronous buffer will transition to
   *             wait as soon as it has read the data from the buffer.
   *  - quit: stops the inner thread and quits. Transition from any state measure state.
   *
   * FSM abstraction:
   *
   *  +------+   set_cells_to_meas   +------+   intra_freq_meas_period_ms   +---------+
   *  | Idle | --------------------->| Wait |------------------------------>| Receive |
   *  +------+                       +------+                               +---------+
   *     ^                              ^                                        |          stop  +------+
   *     |                  Read buffer |                                        |          ----->| Quit |
   *   init                        +---------+    intra_freq_meas_len_ms         |                +------+
   * meas_stop                     | Measure |<----------------------------------+
   *                               +---------+
   */
public:
  /**
   * @brief Describes an interface for reporting new cell measurements
   */
  class meas_itf
  {
  public:
    virtual void cell_meas_reset(uint32_t cc_idx)                                    = 0;
    virtual void new_cell_meas(uint32_t cc_idx, const std::vector<phy_meas_t>& meas) = 0;
  };

  /**
   * @brief Describes the default generic configuration arguments
   */
  struct args_t {
    double   srate_hz          = 0.0;  ///< Sampling rate in Hz, set to 0.0 for maximum
    uint32_t len_ms            = 20;   ///< Amount of time to accumulate
    uint32_t period_ms         = 200;  ///< Accumulation trigger period
    float    rx_gain_offset_db = 0.0f; ///< Gain offset, for calibrated measurements
  };

  /**
   * @brief Stops the operation of this component and it cannot be started again
   * @note use meas_stop() method to stop measurements temporally
   */
  void stop();

  /**
   * @brief Updates the receiver gain offset to convert estimated dBFs to dBm in RSRP
   * @param rx_gain_offset Gain offset in dB
   */
  void set_rx_gain_offset(float rx_gain_offset_db);

  /**
   * @brief Sets the PCI list of the cells this components needs to measure and starts the FSM for measuring
   * @param pci is the list of PCIs to measure
   */
  void set_cells_to_meas(const std::set<uint32_t>& pci);

  /**
   * @brief Stops the measurement FSM, setting the inner state to idle.
   */
  void meas_stop();

  /**
   * @brief Inputs the baseband IQ samples into the component, internal state dictates whether it will be written or
   * not.
   * @param tti The current physical layer TTI, used for calculating the buffer write
   * @param data buffer with baseband IQ samples
   * @param nsamples number of samples to write
   */
  void write(uint32_t tti, cf_t* data, uint32_t nsamples);

  /**
   * @brief Get EARFCN of this component
   * @return EARFCN
   */
  virtual uint32_t get_earfcn() const = 0;

  /**
   * @brief Synchronous wait mechanism, blocks the writer thread while it is in measure state. If the asynchronous
   * thread is too slow, use this method for stalling the writing thread and wait the asynchronous thread to clear the
   * buffer.
   */
  void wait_meas()
  { // Only used by scell_search_test
    state.wait_change(internal_state::measure);
  }

protected:
  struct measure_context_t {
    uint32_t           cc_idx            = 0;    ///< Component carrier index
    float              rx_gain_offset_db = 0.0f; ///< Current gain offset
    std::set<uint32_t> active_pci        = {};   ///< Set with the active PCIs
    uint32_t           sf_len            = 0;    ///< Subframe length in samples
    uint32_t           meas_len_ms       = 20;   ///< Measure length in milliseconds/sub-frames
    uint32_t           meas_period_ms    = 200;  ///< Measure period in milliseconds/sub-frames
    meas_itf&          new_cell_itf;

    explicit measure_context_t(meas_itf& new_cell_itf_) : new_cell_itf(new_cell_itf_) {}
  };

  /**
   * @brief Generic initialization method, necessary to configure main parameters
   * @param cc_idx_ Indicates the component carrier index linked to the intra frequency measurement instance
   * @param args Generic configuration arguments
   */
  void init_generic(uint32_t cc_idx_, const args_t& args);

  /**
   * @brief Constructor is only accessible through inherited classes
   */
  intra_measure_base(srslog::basic_logger& logger, meas_itf& new_cell_itf_);

  /**
   * @brief Destructor is only accessible through inherited classes
   */
  ~intra_measure_base() override;

  /**
   * @brief Subframe length setter, the inherited class shall set the subframe length
   * @param new_sf_len New subframe length
   */
  void set_current_sf_len(uint32_t new_sf_len) { context.sf_len = new_sf_len; }

private:
  /**
   * @brief Describes the internal state class, provides thread safe state management
   */
  class internal_state
  {
  public:
    typedef enum {
      idle = 0, ///< Initial state, internal thread runs, it does not capture data
      wait,     ///< Wait for the period time to pass
      receive,  ///< Accumulate samples in ring buffer
      measure,  ///< Module is busy measuring
      quit      ///< Quit thread, no transitions are allowed
    } state_t;

  private:
    state_t                 state = idle;
    std::mutex              mutex;
    std::condition_variable cvar;

  public:
    /**
     * @brief Get the internal state
     * @return protected state
     */
    state_t get_state() const { return state; }

    /**
     * @brief Transitions to a different state, all transitions are allowed except from quit
     * @param new_state
     */
    void set_state(state_t new_state)
    {
      std::unique_lock<std::mutex> lock(mutex);
      // Do not allow transition from quit
      if (state != quit) {
        state = new_state;
      }

      // Notifies to the inner thread about the change of state
      cvar.notify_all();
    }

    /**
     * @brief Waits for a state transition to a state different than the provided, used for blocking the inner thread
     */
    void wait_change(state_t s)
    {
      std::unique_lock<std::mutex> lock(mutex);
      while (state == s) {
        cvar.wait(lock);
      }
    }
  };

  /**
   * @brief Get the Radio Access Technology (RAT) that is being measured
   * @return The measured RAT
   */
  virtual srsran::srsran_rat_t get_rat() const = 0;

  /**
   * @brief Pure virtual function to perform measurements
   */
  virtual void measure_rat(const measure_context_t& context, std::vector<cf_t>& buffer) = 0;

  /**
   * @brief Measurement process helper method. Encapsulates the neighbour cell measurement functionality
   */
  void measure_proc();

  /**
   * @brief Internal asynchronous low priority thread, waits for measure internal state to execute the measurement
   * process. It stops when the internal state transitions to quit.
   */
  void run_thread() override;

  ///< Internal Thread priority, low by default
  const static int INTRA_FREQ_MEAS_PRIO = DEFAULT_PRIORITY + 5;

  internal_state        state;
  srslog::basic_logger& logger;
  mutable std::mutex    active_pci_mutex = {};
  uint32_t              last_measure_tti = 0;
  measure_context_t     context;

  std::vector<cf_t>   search_buffer;
  srsran_ringbuffer_t ring_buffer = {};
};

} // namespace scell
} // namespace srsue

#endif // SRSUE_INTRA_MEASURE_BASE_H