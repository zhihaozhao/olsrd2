/*
 * dlep_session.h
 *
 *  Created on: Jun 26, 2015
 *      Author: rogge
 */

#ifndef _DLEP_SESSION_H_
#define _DLEP_SESSION_H_

struct dlep_session;
struct dlep_writer;

#include "common/common_types.h"
#include "common/avl.h"
#include "common/autobuf.h"
#include "common/netaddr.h"
#include "subsystems/oonf_layer2.h"
#include "subsystems/oonf_interface.h"
#include "subsystems/oonf_stream_socket.h"
#include "subsystems/oonf_timer.h"
#include "subsystems/os_interface_data.h"

#include "dlep/dlep_extension.h"
#include "dlep/dlep_iana.h"

enum dlep_parser_error {
  DLEP_NEW_PARSER_OKAY                  =  0,
  DLEP_NEW_PARSER_INCOMPLETE_TLV_HEADER = -1,
  DLEP_NEW_PARSER_INCOMPLETE_TLV        = -2,
  DLEP_NEW_PARSER_UNSUPPORTED_TLV       = -3,
  DLEP_NEW_PARSER_ILLEGAL_TLV_LENGTH    = -4,
  DLEP_NEW_PARSER_MISSING_MANDATORY_TLV = -5,
  DLEP_NEW_PARSER_DUPLICATE_TLV         = -6,
  DLEP_NEW_PARSER_OUT_OF_MEMORY         = -7,
  DLEP_NEW_PARSER_INTERNAL_ERROR        = -8,
};

/**
 * Definition of a TLV that has been parsed by DLEP
 */
struct dlep_parser_tlv {
  /*! tlv id */
  uint16_t id;

  /*! index of first session value for tlv, -1 if none */
  int32_t tlv_first;

  /*! index of last session value for tlv, -1 if none */
  int32_t tlv_last;

  /*! minimal length of tlv */
  uint16_t length_min;

  /*! maximal length of tlv */
  uint16_t length_max;

  /*! node for session tlv tree */
  struct avl_node _node;
};

/**
 * header for binary data gathered for a TLV of a certain type
 */
struct dlep_parser_value {
  /*! index of next session value */
  int32_t tlv_next;

  /*! index of value within signal buffer */
  uint16_t index;

  /*! length of tlv in bytes */
  uint16_t length;
};

/**
 * Session for the DLEP tlv parser
 */
struct dlep_session_parser {
  /*! tree of allowed TLVs for this session */
  struct avl_tree allowed_tlvs;

  /*! array of TLV values */
  struct dlep_parser_value *values;

  /*! size of array for TLV value headers */
  size_t value_max_count;

  /*! array of active dlep extensions */
  struct dlep_extension **extensions;

  /*! number of active dlep extensions */
  size_t extension_count;

  /*! start of signal tlvs that is has been parsed */
  const uint8_t *tlv_ptr;

  /*! neighbor MAC a signal is referring to */
  struct netaddr signal_neighbor_mac;
};

/**
 * DLEP writer for TLV data
 */
struct dlep_writer {
  /*! output buffer for binary data */
  struct autobuf *out;

  /*! type of signal */
  uint16_t signal_type;

  /*! pointer to first byte of signal */
  char *signal_start_ptr;
};

enum dlep_neighbor_state {
  DLEP_NEIGHBOR_IDLE       = 0,
  DLEP_NEIGHBOR_UP_SENT    = 1,
  DLEP_NEIGHBOR_UP_ACKED   = 2,
  DLEP_NEIGHBOR_DOWN_SENT  = 3,
  DLEP_NEIGHBOR_DOWN_ACKED = 4,
};

/**
 * Neighbor that has been used in a DLEP session
 */
struct dlep_local_neighbor {
  /**
   * mac address of the endpoint of the neighbor
   * (might be proxied ethernet)
   */
  struct netaddr addr;

  /*! state of neighbor */
  enum dlep_neighbor_state state;

  /*! true if the neighbor changes since the last update */
  bool changed;

  /*! mac address of the neighbors wireless interface */
  struct netaddr neigh_addr;

  /*! back-pointer to dlep session */
  struct dlep_session *session;

  /*! timeout for acknowledgement signal */
  struct oonf_timer_instance _ack_timeout;

  /*! hook into the sessions treee of neighbors */
  struct avl_node _node;
};

/**
 * Configuration of a dlep session
 */
struct dlep_session_config {
  /*! peer type of local session */
  const char *peer_type;

  /*! discovery interval */
  uint64_t discovery_interval;

  /*! heartbeat settings for our heartbeats */
  uint64_t heartbeat_interval;

  /*! true if normal neighbors should be sent with DLEP */
  bool send_neighbors;

  /*! true if proxied neighbors should be sent with DLEP */
  bool send_proxied;
};

/**
 * Generic DLEP session, might be radio or router
 */
struct dlep_session {
  /*! copy of local configuration */
  struct dlep_session_config cfg;

  /*! next expected signal for session */
  enum dlep_signals next_signal;

  /*! true if this is a radio session */
  bool radio;

  /*! parser for this dlep session */
  struct dlep_session_parser parser;

  /*! signal writer*/
  struct dlep_writer writer;

  /*! tree of local neighbors being processed by DLEP */
  struct avl_tree local_neighbor_tree;

  /*! oonf layer2 origin for dlep session */
  uint32_t l2_origin;

  /*! send content of output buffer */
  void (*cb_send_buffer)(struct dlep_session *, int af_family);

  /*! terminate the current session */
  void (*cb_end_session)(struct dlep_session *);

  /*! handle timeout for destination */
  void (*cb_destination_timeout)(struct dlep_session *,
      struct dlep_local_neighbor *);

  /*! log source for usage of this session */
  enum oonf_log_source log_source;

  /*! local layer2 data interface */
  struct oonf_interface_listener l2_listener;

  /*! timer to generate discovery/heartbeats */
  struct oonf_timer_instance local_event_timer;

  /*! keep track of remote heartbeats */
  struct oonf_timer_instance remote_heartbeat_timeout;

  /*! rate of remote heartbeats */
  uint64_t remote_heartbeat_interval;

  /*! remote endpoint of current communication */
  union netaddr_socket remote_socket;

  /*! tree of all dlep sessions of an interface */
  struct avl_node _node;
};

void dlep_session_init(void);

int dlep_session_add(struct dlep_session *session,
    const char *l2_ifname, uint32_t l2_origin,
    struct autobuf *out, bool radio, enum oonf_log_source);
void dlep_session_remove(struct dlep_session *session);
void dlep_session_terminate(struct dlep_session *session);

int dlep_session_update_extensions(struct dlep_session *session,
    const uint8_t *extvalues, size_t extcount);
enum oonf_stream_session_state dlep_session_process_tcp(
    struct oonf_stream_session *tcp_session,
    struct dlep_session *session);
ssize_t dlep_session_process_buffer(
    struct dlep_session *session, const void *buffer, size_t length);
size_t dlep_session_process_signal(struct dlep_session *session,
    const void *buffer, size_t length);
int dlep_session_generate_signal(struct dlep_session *session,
    uint16_t signal, const struct netaddr *neighbor);
int dlep_session_generate_signal_status(struct dlep_session *session,
    uint16_t signal, const struct netaddr *neighbor,
    enum dlep_status status, const char *msg);
struct dlep_parser_value *dlep_session_get_tlv_value(
    struct dlep_session *session, uint16_t tlvtype);

struct dlep_local_neighbor *dlep_session_add_local_neighbor(
    struct dlep_session *session, const struct netaddr *neigh);
void dlep_session_remove_local_neighbor(
    struct dlep_session *session, struct dlep_local_neighbor *local);
struct dlep_local_neighbor *dlep_session_get_local_neighbor(
    struct dlep_session *session, const struct netaddr *neigh);
struct oonf_layer2_neigh *dlep_session_get_local_l2_neighbor(
    struct dlep_session *session, const struct netaddr *neigh);

/**
 * get the dlep session tlv
 * @param parser dlep session parser
 * @param tlvtype tlvtype
 * @return tlv parser information, NULL if not available
 */
static INLINE struct dlep_parser_tlv *
dlep_parser_get_tlv(struct dlep_session_parser *parser, uint16_t tlvtype) {
    struct dlep_parser_tlv *tlv;
  return avl_find_element(&parser->allowed_tlvs, &tlvtype, tlv, _node);
}

/**
 * Get value of first appearance of a TLV
 * @param session dlep session parser
 * @param tlv dlep session tlv
 * @return dlep tlv value, NULL if no value available
 */
static INLINE struct dlep_parser_value *
dlep_session_get_tlv_first_value(struct dlep_session *session,
    struct dlep_parser_tlv *tlv) {
  if (tlv->tlv_first == -1) {
    return NULL;
  }
  return &session->parser.values[tlv->tlv_first];
}

/**
 * Get the next value of a TLV
 * @param session dlep session parser
 * @param value current dlep parser value
 * @return next dlep tlv value, NULL if no further values
 */
static INLINE struct dlep_parser_value *
dlep_session_get_next_tlv_value(struct dlep_session *session,
    struct dlep_parser_value *value) {
  if (value->tlv_next == -1) {
    return NULL;
  }
  return &session->parser.values[value->tlv_next];
}

/**
 * Get the binary data of a tlv
 * @param parser dlep session parser
 * @param value dlep tlv value
 * @return binary data pointer
 */
static INLINE const uint8_t *
dlep_parser_get_tlv_binary(struct dlep_session_parser *parser,
    struct dlep_parser_value *value) {
  return &parser->tlv_ptr[value->index];
}


/**
 * Shortcut for getting the binary data of a TLV for a session
 * @param session dlep session
 * @param value dlep tlv value
 * @return binary data pointer
 */
static INLINE const uint8_t *
dlep_session_get_tlv_binary(struct dlep_session *session,
    struct dlep_parser_value *value) {
  return &session->parser.tlv_ptr[value->index];
}

#endif /* _DLEP_SESSION_H_ */
