
/*
 * The olsr.org Optimized Link-State Routing daemon version 2 (olsrd2)
 * Copyright (c) 2004-2015, the olsr.org team - see HISTORY file
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in
 *   the documentation and/or other materials provided with the
 *   distribution.
 * * Neither the name of olsr.org, olsrd nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Visit http://www.olsr.org for more information.
 *
 * If you find this software useful feel free to make a donation
 * to the project. For more information see the website or contact
 * the copyright holders.
 *
 */

/**
 * @file
 */

#include "common/common_types.h"
#include "common/avl.h"
#include "common/avl_comp.h"
#include "subsystems/oonf_packet_socket.h"

#include "dlep/dlep_extension.h"
#include "dlep/dlep_iana.h"
#include "dlep/dlep_session.h"
#include "dlep/dlep_writer.h"
#include "dlep/dlep_interface.h"

static void _cb_receive_udp(struct oonf_packet_socket *,
    union netaddr_socket *from, void *ptr, size_t length);
static void _cb_send_multicast(struct dlep_session *session, int af_family);

static const char _DLEP_PREFIX[] = DLEP_DRAFT_17_PREFIX;

/**
 * Add a new interface to this dlep instance
 * @param interf pointer to interface
 * @param ifname name of interface
 * @param l2_origin layer2 originator that shall be used
 * @param log_src logging source that shall be used
 * @param radio true if it is a radio interface, false for router
 * @return -1 if an error happened, 0 otherwise
 */
int
dlep_if_add(struct dlep_if *interf, const char *ifname,
    uint32_t l2_origin, enum oonf_log_source log_src, bool radio) {
  struct dlep_extension *ext;

  /* initialize key */
  strscpy(interf->l2_ifname, ifname,
      sizeof(interf->l2_ifname));
  interf->_node.key = interf->l2_ifname;

  if (abuf_init(&interf->udp_out)) {
    return -1;
  }

  /* add dlep prefix to buffer */
  abuf_memcpy(&interf->udp_out,
      _DLEP_PREFIX, sizeof(_DLEP_PREFIX) - 1);

  if (dlep_session_add(&interf->session,
      interf->l2_ifname, l2_origin,
      &interf->udp_out,
      radio, log_src)) {
    abuf_free(&interf->udp_out);
    return -1;
  }

  /* initialize stream list */
  avl_init(&interf->session_tree, avl_comp_netaddr_socket, false);

  /* initialize discovery socket */
  interf->udp.config.user = interf;
  interf->udp.config.receive_data = _cb_receive_udp;
  oonf_packet_add_managed(&interf->udp);

  /* initialize session */
  interf->session.cb_send_buffer = _cb_send_multicast;
  interf->session.cb_end_session = NULL;
  interf->session.restrict_signal =
      radio ? DLEP_PEER_DISCOVERY : DLEP_PEER_OFFER;
  interf->session.writer.out = &interf->udp_out;

  /* inform all extension */
  avl_for_each_element(dlep_extension_get_tree(), ext, _node) {
    if (radio) {
      if (ext->cb_session_init_radio) {
        ext->cb_session_init_radio(&interf->session);
      }
    }
    else {
      if (ext->cb_session_init_router) {
        ext->cb_session_init_router(&interf->session);
      }
    }
  }
  return 0;
}

/**
 * Remove dlep router interface
 * @param interface dlep router interface
 */
void
dlep_if_remove(struct dlep_if *interface) {
  struct dlep_extension *ext;

  OONF_DEBUG(interface->session.log_source,
      "remove session %s", interface->l2_ifname);

  avl_for_each_element(dlep_extension_get_tree(), ext, _node) {
    if (interface->session.radio) {
      if (ext->cb_session_cleanup_radio) {
        ext->cb_session_cleanup_radio(&interface->session);
      }
    }
    else {
      if (ext->cb_session_cleanup_router) {
        ext->cb_session_cleanup_router(&interface->session);
      }
    }
  }

  /* close UDP interface */
  oonf_packet_remove_managed(&interface->udp, true);

  /* kill dlep session */
  dlep_session_remove(&interface->session);
}
/**
 * Callback to receive UDP data through oonf_packet_managed API
 * @param pkt packet socket
 * @param from network socket the packet was received from
 * @param ptr pointer to packet data
 * @param length length of packet data
 */
static void
_cb_receive_udp(struct oonf_packet_socket *pkt,
    union netaddr_socket *from, void *ptr, size_t length) {
  struct dlep_if *interf;
  uint8_t *buffer;
  ssize_t processed;
  struct netaddr_str nbuf;

  interf = pkt->config.user;
  buffer = ptr;

  if (interf->session_tree.count > 0
      && interf->single_session) {
    /* ignore UDP traffic as long as we have a connection */
    return;
  }

  if (length < sizeof(_DLEP_PREFIX) - 1) {
    /* ignore unknown prefix */
    return;
  }

  if (netaddr_socket_cmp(from, &pkt->local_socket) == 0) {
    /* we hear outselves, ignore it */
    return;
  }

  if (memcmp(buffer, _DLEP_PREFIX, sizeof(_DLEP_PREFIX)-1) != 0) {
    OONF_WARN(interf->session.log_source,
        "Incoming UDP packet with unknown signature");
    return;
  }

  /* advance pointer and fix length */
  buffer += (sizeof(_DLEP_PREFIX) - 1);
  length -= (sizeof(_DLEP_PREFIX) - 1);

  /* copy socket information */
  memcpy(&interf->session.remote_socket, from,
      sizeof(interf->session.remote_socket));

  processed = dlep_session_process_buffer(&interf->session, buffer, length);
  if (processed < 0) {
    return ;
  }

  if ((size_t)processed < length) {
    OONF_WARN(interf->session.log_source,
        "Received malformed or too short UDP packet from %s",
        netaddr_socket_to_string(&nbuf, from));
    /* incomplete or bad packet, just ignore it */
    return;
  }

  if (abuf_getlen(interf->session.writer.out) > sizeof(_DLEP_PREFIX) - 1) {
    /* send an unicast response */
    oonf_packet_send_managed(&interf->udp, from,
        abuf_getptr(interf->session.writer.out),
        abuf_getlen(interf->session.writer.out));
    abuf_clear(interf->session.writer.out);

    /* add dlep prefix to buffer */
    abuf_memcpy(interf->session.writer.out,
        _DLEP_PREFIX, sizeof(_DLEP_PREFIX) - 1);
  }

  netaddr_socket_invalidate(&interf->session.remote_socket);
}

/**
 * Callback to send multicast over interface
 * @param session dlep session
 * @param af_family address family for multicast
 */
static void
_cb_send_multicast(struct dlep_session *session, int af_family) {
  struct dlep_if *interf;

  if (abuf_getlen(session->writer.out) <= sizeof(_DLEP_PREFIX) - 1
      || !netaddr_socket_is_unspec(&session->remote_socket)) {
    return;
  }

  /* get pointer to radio interface */
  interf = container_of(session, struct dlep_if, session);

  if (interf->session_tree.count > 0
      && interf->single_session) {
    /* do not produce UDP traffic as long as we are connected */
    return;
  }

  OONF_DEBUG(session->log_source, "Send multicast %"
      PRINTF_SIZE_T_SPECIFIER " bytes",
      abuf_getlen(session->writer.out));

  oonf_packet_send_managed_multicast(&interf->udp,
      abuf_getptr(session->writer.out), abuf_getlen(session->writer.out),
      af_family);

  abuf_clear(session->writer.out);

  /* add dlep prefix to buffer */
  abuf_memcpy(session->writer.out, _DLEP_PREFIX, sizeof(_DLEP_PREFIX) - 1);
}
