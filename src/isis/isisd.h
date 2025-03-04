/*
 * IS-IS Rout(e)ing protocol - isisd.h   
 *
 * Copyright (C) 2001,2002   Sampo Saaristo
 *                           Tampere University of Technology      
 *                           Institute of Communications Engineering
 *
 * This program is free software; you can redistribute it and/or modify it 
 * under the terms of the GNU General Public Licenseas published by the Free 
 * Software Foundation; either version 2 of the License, or (at your option) 
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,but WITHOUT 
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or 
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for 
 * more details.

 * You should have received a copy of the GNU General Public License along 
 * with this program; if not, write to the Free Software Foundation, Inc., 
 * 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#ifndef _ISISD_H_
#define _ISISD_H_

#define ISISD_VERSION "0.0.7"
#define ZEBRA_ROUTE_MAX 11

/* uncomment if you are a developer in bug hunt */

struct rmap
{
  char *name;
  struct route_map *map;
};

struct isis
{
  u_long process_id;
  int sysid_set;
  u_char sysid[ISIS_SYS_ID_LEN];	/* SystemID for this IS */
  struct list *area_list;	/* list of IS-IS areas */
  struct list *init_circ_list;
  struct list *nexthops;	/* IPv4 next hops from this IS */
  struct list *nexthops6;	/* IPv6 next hops from this IS */
  u_char max_area_addrs;	/* maximumAreaAdresses */
  struct area_addr *man_area_addrs;	/* manualAreaAddresses */
  u_int32_t debugs;		/* bitmap for debug */
  time_t uptime;		/* when did we start */
  struct thread *t_dync_clean;	/* dynamic hostname cache cleanup thread */

  /* Redistributed external information. */
  struct route_table *external_info[ZEBRA_ROUTE_MAX + 1];
  /* Redistribute metric info. */
  struct
  {
    int type;			/* Internal or External  */
    int value;			/* metric value */
  } dmetric[ZEBRA_ROUTE_MAX + 1];

  struct
  {
    char *name;
    struct route_map *map;
  } rmap[ZEBRA_ROUTE_MAX + 1];
  struct
  {
    struct
    {
      char *name;
      struct route_map *map;
    } rmap[ZEBRA_ROUTE_MAX + 1];
  } inet6_afmode;
};

struct isis_area
{
  struct isis *isis;				  /* back pointer */
  dict_t *lspdb[ISIS_LEVELS];			  /* link-state dbs */
  struct isis_spftree *spftree[ISIS_LEVELS];	  /* The v4 SPTs */
  struct route_table *route_table[ISIS_LEVELS];	  /* IPv4 routes */
  struct isis_spftree *spftree6[ISIS_LEVELS];	  /* The v6 SPTs */
  struct route_table *route_table6[ISIS_LEVELS];  /* IPv6 routes */
  unsigned int min_bcast_mtu;
  struct list *circuit_list;	/* IS-IS circuits */
  struct flags flags;
  struct thread *t_tick;	/* LSP walker */
  struct thread *t_remove_aged;
  struct thread *t_lsp_l1_regenerate;
  struct thread *t_lsp_l2_regenerate;
  int lsp_regenerate_pending[ISIS_LEVELS];
  struct thread *t_lsp_refresh[ISIS_LEVELS];

  /*
   * Configurables 
   */
  struct isis_passwd area_passwd;
  struct isis_passwd domain_passwd;
  /* do we support dynamic hostnames?  */
  char dynhostname;
  /* do we support new style metrics?  */
  char newmetric;
  char oldmetric;
  /* identifies the routing instance   */
  char *area_tag;
  /* area addresses for this area      */
  struct list *area_addrs;
  u_int16_t max_lsp_lifetime[ISIS_LEVELS];
  char is_type;			/* level-1 level-1-2 or level-2-only */
  u_int16_t lsp_refresh[ISIS_LEVELS];
  /* minimum time allowed before lsp retransmission */
  u_int16_t lsp_gen_interval[ISIS_LEVELS];
  /* min interval between between consequtive SPFs */
  u_int16_t min_spf_interval[ISIS_LEVELS];
  /* the percentage of LSP mtu size used, before generating a new frag */
  int lsp_frag_threshold;
  int ip_circuits;
  int ipv6_circuits;
  /* Counters */
  u_int32_t circuit_state_changes;
};

extern void isis_init (void);
extern struct isis_area *isis_area_lookup (const char *);
extern struct isis_area *isis_area_create ();
extern int area_net_title(struct isis_area *, const u_char *);

#define DEBUG_ADJ_PACKETS                (1<<0)
#define DEBUG_CHECKSUM_ERRORS            (1<<1)
#define DEBUG_LOCAL_UPDATES              (1<<2)
#define DEBUG_PROTOCOL_ERRORS            (1<<3)
#define DEBUG_SNP_PACKETS                (1<<4)
#define DEBUG_UPDATE_PACKETS             (1<<5)
#define DEBUG_SPF_EVENTS                 (1<<6)
#define DEBUG_SPF_STATS                  (1<<7)
#define DEBUG_SPF_TRIGGERS               (1<<8)
#define DEBUG_RTE_EVENTS                 (1<<9)
#define DEBUG_EVENTS                     (1<<10)
#define DEBUG_ZEBRA                      (1<<11)

#endif /* _ISISD_H_ */
