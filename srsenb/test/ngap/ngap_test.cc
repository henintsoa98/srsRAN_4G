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

#include "srsenb/hdr/stack/ngap/ngap.h"
#include "srsran/common/network_utils.h"
#include "srsran/common/test_common.h"

using namespace srsenb;

struct amf_dummy {
  amf_dummy(const char* addr_str_, int port_) : addr_str(addr_str_), port(port_)
  {
    srsran::net_utils::set_sockaddr(&amf_sockaddr, addr_str, port);
    {
      using namespace srsran::net_utils;
      fd = open_socket(addr_family::ipv4, socket_type::seqpacket, protocol_type::SCTP);
      TESTASSERT(fd > 0);
      TESTASSERT(bind_addr(fd, amf_sockaddr));
    }

    int success = listen(fd, SOMAXCONN);
    srsran_assert(success == 0, "Failed to listen to incoming SCTP connections");
  }

  ~amf_dummy()
  {
    if (fd > 0) {
      close(fd);
    }
  }

  srsran::unique_byte_buffer_t read_msg(sockaddr_in* sockfrom = nullptr)
  {
    srsran::unique_byte_buffer_t pdu     = srsran::make_byte_buffer();
    sockaddr_in                  from    = {};
    socklen_t                    fromlen = sizeof(from);
    sctp_sndrcvinfo              sri     = {};
    int                          flags   = 0;
    ssize_t n_recv = sctp_recvmsg(fd, pdu->msg, pdu->get_tailroom(), (struct sockaddr*)&from, &fromlen, &sri, &flags);
    if (n_recv > 0) {
      if (sockfrom != nullptr) {
        *sockfrom = from;
      }
      pdu->N_bytes = n_recv;
    }
    return pdu;
  }

  const char*                  addr_str;
  int                          port;
  struct sockaddr_in           amf_sockaddr = {};
  int                          fd;
  srsran::unique_byte_buffer_t last_sdu;
};

struct dummy_socket_manager : public srsran::socket_manager_itf {
  dummy_socket_manager() : srsran::socket_manager_itf(srslog::fetch_basic_logger("TEST")) {}

  /// Register (fd, callback). callback is called within socket thread when fd has data.
  bool add_socket_handler(int fd, recv_callback_t handler) final
  {
    if (s1u_fd > 0) {
      return false;
    }
    s1u_fd   = fd;
    callback = std::move(handler);
    return true;
  }

  /// remove registered socket fd
  bool remove_socket(int fd) final
  {
    if (s1u_fd < 0) {
      return false;
    }
    s1u_fd = -1;
    return true;
  }

  int             s1u_fd;
  recv_callback_t callback;
};

void run_ng_setup(ngap& ngap_obj, amf_dummy& amf)
{
  asn1::ngap_nr::ngap_pdu_c ngap_pdu;

  // gNB -> AMF: NG Setup Request
  srsran::unique_byte_buffer_t sdu = amf.read_msg();
  TESTASSERT(sdu->N_bytes > 0);
  asn1::cbit_ref cbref(sdu->msg, sdu->N_bytes);
  TESTASSERT(ngap_pdu.unpack(cbref) == asn1::SRSASN_SUCCESS);
  TESTASSERT(ngap_pdu.type().value == asn1::ngap_nr::ngap_pdu_c::types_opts::init_msg);
  TESTASSERT(ngap_pdu.init_msg().proc_code == ASN1_NGAP_NR_ID_NG_SETUP);

  // AMF -> gNB: ng Setup Response
  sockaddr_in     amf_addr = {};
  sctp_sndrcvinfo rcvinfo  = {};
  int             flags    = 0;

  uint8_t ng_setup_resp[] = {0x20, 0x15, 0x00, 0x55, 0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x31, 0x17, 0x00, 0x61, 0x6d,
                             0x61, 0x72, 0x69, 0x73, 0x6f, 0x66, 0x74, 0x2e, 0x61, 0x6d, 0x66, 0x2e, 0x35, 0x67, 0x63,
                             0x2e, 0x6d, 0x6e, 0x63, 0x30, 0x30, 0x31, 0x2e, 0x6d, 0x63, 0x63, 0x30, 0x30, 0x31, 0x2e,
                             0x33, 0x67, 0x70, 0x70, 0x6e, 0x65, 0x74, 0x77, 0x6f, 0x72, 0x6b, 0x2e, 0x6f, 0x72, 0x67,
                             0x00, 0x60, 0x00, 0x08, 0x00, 0x00, 0x00, 0xf1, 0x10, 0x80, 0x01, 0x01, 0x00, 0x56, 0x40,
                             0x01, 0x32, 0x00, 0x50, 0x00, 0x08, 0x00, 0x00, 0xf1, 0x10, 0x00, 0x00, 0x00, 0x08};
  memcpy(sdu->msg, ng_setup_resp, sizeof(ng_setup_resp));
  sdu->N_bytes = sizeof(ng_setup_resp);
  TESTASSERT(ngap_obj.handle_amf_rx_msg(std::move(sdu), amf_addr, rcvinfo, flags));
}

int main(int argc, char** argv)
{
  // Setup logging.
  auto& logger = srslog::fetch_basic_logger("NGAP");
  logger.set_level(srslog::basic_levels::debug);
  logger.set_hex_dump_max_size(-1);

  srsran::task_scheduler task_sched;
  dummy_socket_manager   rx_sockets;
  ngap                   ngap_obj(&task_sched, logger, &rx_sockets);

  const char*    amf_addr_str = "127.0.0.1";
  const uint32_t AMF_PORT     = 38412;
  amf_dummy      amf(amf_addr_str, AMF_PORT);

  ngap_args_t args   = {};
  args.cell_id       = 0x01;
  args.gnb_id        = 0x19B;
  args.mcc           = 907;
  args.mnc           = 70;
  args.ngc_bind_addr = "127.0.0.100";
  args.tac           = 7;
  args.gtp_bind_addr = "127.0.0.100";
  args.amf_addr      = amf_addr_str;
  args.gnb_name      = "srsgnb01";
  rrc_interface_ngap_nr rrc;
  ngap_obj.init(args, &rrc);

  // Start the log backend.
  srsran::test_init(argc, argv);
  run_ng_setup(ngap_obj, amf);
}