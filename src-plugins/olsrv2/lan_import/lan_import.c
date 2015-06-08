
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

#include "common/autobuf.h"
#include "common/avl.h"
#include "common/avl_comp.h"
#include "common/common_types.h"
#include "common/list.h"
#include "common/netaddr.h"
#include "common/netaddr_acl.h"

#include "core/oonf_logging.h"
#include "core/oonf_subsystem.h"
#include "subsystems/oonf_class.h"

#include "olsrv2/olsrv2.h"
#include "olsrv2/olsrv2_lan.h"
#include "olsrv2/olsrv2_routing.h"

#include "lan_import/lan_import.h"

/* definitions */
#define LOG_LAN_IMPORT _import_subsystem.logging

struct _import_entry {
  /* name of the lan import */
  char name[16];

  /* domain of the lan import */
  int32_t domain;

  /* address filter */
  struct netaddr_acl filter;
  int32_t prefix_length;

  /* interface name filter */
  char ifname[IF_NAMESIZE];

  /* additional filters for lan event, 0 if not matched */
  int32_t table;
  int32_t protocol;
  int32_t distance;

  /* tree of all configured lan import */
  struct avl_node _node;
};

/* prototypes */
static int _init(void);
static void _cleanup(void);

static struct _import_entry *_get_import(const char *name);
static void _destroy_import(struct _import_entry *);

static void _cb_query(struct os_route *filter, struct os_route *route);
static void _cb_query_finished(struct os_route *, int error);
static void _cb_rt_event(const struct os_route *, bool);
static void _cb_cfg_changed(void);

/* plugin declaration */
static struct cfg_schema_entry _import_entries[] = {
  CFG_MAP_INT32_MINMAX(_import_entry, domain, "domain", "0",
      "Routing domain id for filter", 0, false, 0, 255),
  CFG_MAP_ACL(_import_entry, filter, "matches",
      OLSRV2_ROUTABLE_IPV4 OLSRV2_ROUTABLE_IPV6 ACL_DEFAULT_ACCEPT,
      "Ip addresses the filter should be applied to"),
  CFG_MAP_INT32_MINMAX(_import_entry, prefix_length, "prefix_length", "-1",
      "Prefix length the filter should be applied to, -1 for any prefix length",
      0, false, -1, 128),
  CFG_MAP_STRING_ARRAY(_import_entry, ifname, "interface", "",
      "Interface name of matching routes, empty if all interfaces", IF_NAMESIZE),
  CFG_MAP_INT32_MINMAX(_import_entry, table, "table", "0",
      "Routing table of matching routes", 0, false, 0, 255),
  CFG_MAP_INT32_MINMAX(_import_entry, protocol, "protocol", "0",
      "Routing protocol of matching routes", 0, false, 0, 255),
  CFG_MAP_INT32_MINMAX(_import_entry, distance, "metric", "0",
      "Metric of matching routes", 0, false, 0, INT32_MAX),
};

static struct cfg_schema_section _import_section = {
  .type = OONF_LAN_IMPORT_SUBSYSTEM,
  .mode = CFG_SSMODE_NAMED,
  .def_name = "0",

  .cb_delta_handler = _cb_cfg_changed,

  .entries = _import_entries,
  .entry_count = ARRAYSIZE(_import_entries),
};

static const char *_dependencies[] = {
  OONF_CLASS_SUBSYSTEM,
  OONF_OLSRV2_SUBSYSTEM,
  OONF_OS_ROUTING_SUBSYSTEM,
};
static struct oonf_subsystem _import_subsystem = {
  .name = OONF_LAN_IMPORT_SUBSYSTEM,
  .dependencies = _dependencies,
  .dependencies_count = ARRAYSIZE(_dependencies),
  .descr = "OLSRv2 lan-import plugin",
  .author = "Henning Rogge",

  .cfg_section = &_import_section,

  .init = _init,
  .cleanup = _cleanup,
};
DECLARE_OONF_PLUGIN(_import_subsystem);

/* class definition for filters */
static struct oonf_class _import_class = {
  .name = "lan import",
  .size = sizeof(struct _import_entry),
};

/* callback filter for dijkstra */
static struct os_route_listener _routing_listener = {
  .cb_get = _cb_rt_event,
};

/* tree of lan importers */
static struct avl_tree _import_tree;

/* wildcard route for first query */
static struct os_route _wildcard_query;

/**
 * Initialize plugin
 * @return always returns 0 (cannot fail)
 */
static int
_init(void) {
  avl_init(&_import_tree, avl_comp_strcasecmp, false);
  oonf_class_add(&_import_class);
  os_routing_listener_add(&_routing_listener);

  /* send wildcard query */
  memcpy(&_wildcard_query, os_routing_get_wildcard_route(),
      sizeof(_wildcard_query));
  _wildcard_query.cb_get = _cb_query;
  _wildcard_query.cb_finished = _cb_query_finished;

  return 0;
}

/**
 * Cleanup plugin
 */
static void
_cleanup(void) {
  struct _import_entry *import, *import_it;

  avl_for_each_element_safe(&_import_tree, import, _node, import_it) {
    _destroy_import(import);
  }

  os_routing_listener_remove(&_routing_listener);
  oonf_class_remove(&_import_class);
}

/**
 * Wrapper for cb_get for wildcard query
 * @param filter unused filter
 * @param route route found by wildcard query
 */
static void
_cb_query(struct os_route *filter __attribute__((unused)), struct os_route *route) {
  _cb_rt_event(route, true);
}

/**
 * Dummy cb_finished callback for wildcard query
 * @param route
 * @param error
 */
static void
_cb_query_finished(struct os_route *route __attribute__((unused)),
    int error __attribute__((unused))) {
}

/**
 * Callback for route listener
 * @param route routing data
 * @param set true if route was set, false otherwise
 */
static void
_cb_rt_event(const struct os_route *route, bool set) {
  struct _import_entry *import;
  struct nhdp_domain *domain;
  char ifname[IF_NAMESIZE];

#ifdef OONF_LOG_DEBUG_INFO
  struct os_route_str rbuf;
#endif

  if (netaddr_is_in_subnet(&NETADDR_IPV4_MULTICAST, &route->dst)
      || netaddr_is_in_subnet(&NETADDR_IPV4_LINKLOCAL, &route->dst)
      || netaddr_is_in_subnet(&NETADDR_IPV4_LOOPBACK_NET, &route->dst)
      || netaddr_is_in_subnet(&NETADDR_IPV6_MULTICAST, &route->dst)
      || netaddr_is_in_subnet(&NETADDR_IPV6_LINKLOCAL, &route->dst)
      || netaddr_is_in_subnet(&NETADDR_IPV6_LOOPBACK, &route->dst)) {
    /* ignore multicast, linklocal and loopback */
    return;
  }
  OONF_DEBUG(LOG_LAN_IMPORT, "Received route event (%s): %s",
      set ? "set" : "remove", os_routing_to_string(&rbuf, route));

  avl_for_each_element(&_import_tree, import, _node) {
    OONF_DEBUG(LOG_LAN_IMPORT, "Check for import: %s", import->name);

    /* check prefix length */
    if (import->prefix_length != -1
        && import->prefix_length != netaddr_get_prefix_length(&route->dst)) {
      OONF_DEBUG(LOG_LAN_IMPORT, "Bad prefix length");
      continue;
    }

    /* check if destination matches */
    if (!netaddr_acl_check_accept(&import->filter, &route->dst)) {
      OONF_DEBUG(LOG_LAN_IMPORT, "Bad prefix");
      continue;
    }

    /* check routing table */
    if (import->table && import->table != route->table) {
      OONF_DEBUG(LOG_LAN_IMPORT, "Bad routing table");
      continue;
    }

    /* check protocol */
    if (import->protocol && import->protocol != route->protocol) {
      OONF_DEBUG(LOG_LAN_IMPORT, "Bad protocol");
      continue;
    }

    /* check metric */
    if (import->distance && import->distance != route->metric) {
      OONF_DEBUG(LOG_LAN_IMPORT, "Bad distance");
      continue;
    }

    /* check interface name */
    if (import->ifname[0]) {
      if_indextoname(route->if_index, ifname);
      if (strcmp(import->ifname, ifname) != 0) {
        OONF_DEBUG(LOG_LAN_IMPORT, "Bad interface");
        continue;
      }
    }

    if (set) {
      list_for_each_element(nhdp_domain_get_list(), domain, _node) {
        if (olsrv2_routing_get_parameters(domain)->protocol == route->protocol) {
          /* do never set a LAN for a route tagged with an olsrv2 protocol */
          OONF_DEBUG(LOG_LAN_IMPORT, "Matches olsrv2 protocol!");
          continue;
        }
      }

      OONF_DEBUG(LOG_LAN_IMPORT, "Add lan...");
      olsrv2_lan_add(domain, &route->dst, 1, route->metric);
    }
    else {
      OONF_DEBUG(LOG_LAN_IMPORT, "Remove lan...");
      olsrv2_lan_remove(domain, &route->dst);
    }
  }
}

/**
 * Lookups a lan importer or create a new one
 * @param name name of lan importer
 * @return pointer to lan importer or NULL if out of memory
 */
static struct _import_entry *
_get_import(const char *name) {
  struct _import_entry *import;

  import = avl_find_element(&_import_tree, name, import, _node);
  if (import) {
    return import;
  }

  import = oonf_class_malloc(&_import_class);
  if (import == NULL) {
    return NULL;
  }

  /* copy key and add to tree */
  strscpy(import->name, name, sizeof(import->name));
  import->_node.key = import->name;
  avl_insert(&_import_tree, &import->_node);

  return import;
}

/**
 * Free all resources associated with a route modifier
 * @param import import entry
 */
static void
_destroy_import(struct _import_entry *import) {
  avl_remove(&_import_tree, &import->_node);
  netaddr_acl_remove(&import->filter);
  oonf_class_free(&_import_class, import);
}

/**
 * Configuration changed
 */
static void
_cb_cfg_changed(void) {
  struct _import_entry *import;

  /* get existing modifier */
  import = _get_import(_import_section.section_name);
  if (!import) {
    /* out of memory */
    return;
  }

  if (_import_section.post == NULL) {
    /* section was removed */
    _destroy_import(import);
    return;
  }

  if (cfg_schema_tobin(import, _import_section.post,
      _import_entries, ARRAYSIZE(_import_entries))) {
    OONF_WARN(LOG_LAN_IMPORT,
        "Could not convert configuration data of section '%s'",
        _import_section.section_name);

    if (_import_section.pre == NULL) {
      _destroy_import(import);
    }
    return;
  }

  /* trigger wildcard query */
  if (!os_routing_is_in_progress(&_wildcard_query)) {
    os_routing_query(&_wildcard_query);
  }
}