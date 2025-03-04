/*  
    pmacct (Promiscuous mode IP Accounting package)
    pmacct is Copyright (C) 2003-2019 by Paolo Lucente
*/

/*
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

/* includes */
#include "pmacct.h"
#include "addr.h"
#include "bgp/bgp.h"
#include "bmp.h"
#if defined WITH_RABBITMQ
#include "amqp_common.h"
#endif
#ifdef WITH_KAFKA
#include "kafka_common.h"
#endif

u_int32_t bmp_process_packet(char *bmp_packet, u_int32_t len, struct bmp_peer *bmpp)
{
  struct bgp_misc_structs *bms;
  struct bgp_peer *peer;
  char *bmp_packet_ptr = bmp_packet;
  u_int32_t pkt_remaining_len, orig_msg_len, msg_len, msg_start_len;

  struct bmp_common_hdr *bch = NULL;

  if (!bmpp) return FALSE;

  peer = &bmpp->self;
  bms = bgp_select_misc_db(peer->type);

  if (!bms) return FALSE;

  if (len < sizeof(struct bmp_common_hdr)) {
    Log(LOG_INFO, "INFO ( %s/%s ): [%s] packet discarded: failed bmp_get_and_check_length() BMP common hdr\n",
	config.name, bms->log_str, peer->addr_str);
    return FALSE;
  }

  for (msg_start_len = pkt_remaining_len = len; pkt_remaining_len; msg_start_len = pkt_remaining_len) {
    if (!(bch = (struct bmp_common_hdr *) bmp_get_and_check_length(&bmp_packet_ptr, &pkt_remaining_len, sizeof(struct bmp_common_hdr))))
      return msg_start_len;

    if (bch->version != BMP_V3) {
      Log(LOG_INFO, "INFO ( %s/%s ): [%s] packet discarded: BMP version != %u\n",
	  config.name, bms->log_str, peer->addr_str, BMP_V3);
      return FALSE;
    }

    bmp_common_hdr_get_len(bch, &msg_len);
    msg_len -= sizeof(struct bmp_common_hdr);
    orig_msg_len = msg_len;

    if (pkt_remaining_len < msg_len) return msg_start_len;

    if (bch->type <= BMP_MSG_TYPE_MAX) {
      Log(LOG_DEBUG, "DEBUG ( %s/%s ): [%s] [common] type: %s (%u)\n",
	  config.name, bms->log_str, peer->addr_str, bmp_msg_types[bch->type], bch->type);
    }

    switch (bch->type) {
    case BMP_MSG_ROUTE_MONITOR:
      bmp_process_msg_route_monitor(&bmp_packet_ptr, &msg_len, bmpp);
      break;
    case BMP_MSG_STATS:
      bmp_process_msg_stats(&bmp_packet_ptr, &msg_len, bmpp);
      break;
    case BMP_MSG_PEER_DOWN:
      bmp_process_msg_peer_down(&bmp_packet_ptr, &msg_len, bmpp);
      break;
    case BMP_MSG_PEER_UP:
      bmp_process_msg_peer_up(&bmp_packet_ptr, &msg_len, bmpp); 
      break;
    case BMP_MSG_INIT:
      bmp_process_msg_init(&bmp_packet_ptr, &msg_len, bmpp); 
      break;
    case BMP_MSG_TERM:
      bmp_process_msg_term(&bmp_packet_ptr, &msg_len, bmpp); 
      break;
    case BMP_MSG_ROUTE_MIRROR:
      bmp_process_msg_route_mirror(&bmp_packet_ptr, &msg_len, bmpp);
      break;
    default:
      Log(LOG_INFO, "INFO ( %s/%s ): [%s] packet discarded: unknown message type (%u)\n",
	  config.name, bms->log_str, peer->addr_str, bch->type);
      break;
    }

    /* sync-up status of pkt_remaining_len to bmp_packet_ptr */
    pkt_remaining_len -= (orig_msg_len - msg_len);
 
    if (msg_len) {
      /* let's jump forward: we may have been unable to parse some (sub-)element */
      bmp_jump_offset(&bmp_packet_ptr, &pkt_remaining_len, msg_len);
    }
  }

  return FALSE;
}

void bmp_process_msg_init(char **bmp_packet, u_int32_t *len, struct bmp_peer *bmpp)
{
  struct bgp_misc_structs *bms;
  struct bgp_peer *peer;
  struct bmp_data bdata;
  struct bmp_tlv_hdr *bth;
  u_int16_t bmp_init_type, bmp_init_len;
  char *bmp_init_info;

  struct bmp_log_init_array blinit;

  if (!bmpp) return;

  peer = &bmpp->self;
  bms = bgp_select_misc_db(peer->type);

  if (!bms) return;

  memset(&bdata, 0, sizeof(bdata));
  blinit.entries = 0;

  gettimeofday(&bdata.tstamp, NULL);

  while ((*len)) {
    if (!(bth = (struct bmp_tlv_hdr *) bmp_get_and_check_length(bmp_packet, len, sizeof(struct bmp_tlv_hdr)))) {
      Log(LOG_INFO, "INFO ( %s/%s ): [%s] [init] packet discarded: failed bmp_get_and_check_length() BMP TLV hdr\n",
		config.name, bms->log_str, peer->addr_str);
      return;
    }

    bmp_tlv_hdr_get_type(bth, &bmp_init_type);
    bmp_tlv_hdr_get_len(bth, &bmp_init_len);

    if (!(bmp_init_info = bmp_get_and_check_length(bmp_packet, len, bmp_init_len))) {
      Log(LOG_INFO, "INFO ( %s/%s ): [%s] [init] packet discarded: failed bmp_get_and_check_length() BMP TLV info\n",
		config.name, bms->log_str, peer->addr_str);
      return;
    }

    blinit.e[blinit.entries].type = bmp_init_type;
    blinit.e[blinit.entries].len = bmp_init_len;
    blinit.e[blinit.entries].val = bmp_init_info;

    blinit.entries = bmp_tlv_array_increment(blinit.entries, BMP_INIT_INFO_ENTRIES);
  }

  if (bms->msglog_backend_methods) {
    char event_type[] = "log";

    bmp_log_msg(peer, &bdata, &blinit, bgp_peer_log_seq_get(&bms->log_seq), event_type, config.nfacctd_bmp_msglog_output, BMP_LOG_TYPE_INIT);
  }

  if (bms->dump_backend_methods) bmp_dump_se_ll_append(peer, &bdata, &blinit, BMP_LOG_TYPE_INIT);

  if (bms->msglog_backend_methods || bms->dump_backend_methods) bgp_peer_log_seq_increment(&bms->log_seq);
}

void bmp_process_msg_term(char **bmp_packet, u_int32_t *len, struct bmp_peer *bmpp)
{
  struct bgp_misc_structs *bms;
  struct bgp_peer *peer;
  struct bmp_data bdata;
  struct bmp_tlv_hdr *bth;
  u_int16_t bmp_term_type, bmp_term_len, reason_type = 0;
  char *bmp_term_info;

  struct bmp_log_term_array blterm;

  if (!bmpp) return;

  peer = &bmpp->self;
  bms = bgp_select_misc_db(peer->type);

  if (!bms) return;

  memset(&bdata, 0, sizeof(bdata));
  blterm.entries = 0;

  gettimeofday(&bdata.tstamp, NULL);

  while ((*len)) {
    if (!(bth = (struct bmp_tlv_hdr *) bmp_get_and_check_length(bmp_packet, len, sizeof(struct bmp_tlv_hdr)))) {
      Log(LOG_INFO, "INFO ( %s/%s ): [%s] [term] packet discarded: failed bmp_get_and_check_length() BMP TLV hdr\n",
		config.name, bms->log_str, peer->addr_str);
       return;
    }

    bmp_tlv_hdr_get_type(bth, &bmp_term_type);
    bmp_tlv_hdr_get_len(bth, &bmp_term_len);

    if (!(bmp_term_info = bmp_get_and_check_length(bmp_packet, len, bmp_term_len))) {
      Log(LOG_INFO, "INFO ( %s/%s ): [%s] [term] packet discarded: failed bmp_get_and_check_length() BMP TLV info\n",
		config.name, bms->log_str, peer->addr_str);
      return;
    }

    if (bmp_term_type == BMP_TERM_INFO_REASON && bmp_term_len == 2) bmp_term_hdr_get_reason_type(bmp_packet, len, &reason_type);

    blterm.e[blterm.entries].type = bmp_term_type;
    blterm.e[blterm.entries].len = bmp_term_len;
    blterm.e[blterm.entries].val = bmp_term_info;
    blterm.e[blterm.entries].reas_type = reason_type;

    blterm.entries = bmp_tlv_array_increment(blterm.entries, BMP_TERM_INFO_ENTRIES);
  }

  if (bms->msglog_backend_methods) {
    char event_type[] = "log";

    bmp_log_msg(peer, &bdata, &blterm, bgp_peer_log_seq_get(&bms->log_seq), event_type, config.nfacctd_bmp_msglog_output, BMP_LOG_TYPE_TERM);
  }

  if (bms->dump_backend_methods) bmp_dump_se_ll_append(peer, &bdata, &blterm, BMP_LOG_TYPE_TERM);

  if (bms->msglog_backend_methods || bms->dump_backend_methods) bgp_peer_log_seq_increment(&bms->log_seq);

  /* BGP peers are deleted as part of bmp_peer_close() */
}

void bmp_process_msg_peer_up(char **bmp_packet, u_int32_t *len, struct bmp_peer *bmpp)
{
  struct bgp_misc_structs *bms;
  struct bgp_peer *peer;
  struct bmp_data bdata;
  struct bmp_peer_hdr *bph;
  struct bmp_peer_up_hdr *bpuh;

  if (!bmpp) return;

  peer = &bmpp->self;
  bms = bgp_select_misc_db(peer->type);

  if (!bms) return;

  memset(&bdata, 0, sizeof(bdata));

  if (!(bph = (struct bmp_peer_hdr *) bmp_get_and_check_length(bmp_packet, len, sizeof(struct bmp_peer_hdr)))) {
    Log(LOG_INFO, "INFO ( %s/%s ): [%s] [peer up] packet discarded: failed bmp_get_and_check_length() BMP peer hdr\n",
	config.name, bms->log_str, peer->addr_str);
    return;
  }

  if (!(bpuh = (struct bmp_peer_up_hdr *) bmp_get_and_check_length(bmp_packet, len, sizeof(struct bmp_peer_up_hdr)))) {
    Log(LOG_INFO, "INFO ( %s/%s ): [%s] [peer up] packet discarded: failed bmp_get_and_check_length() BMP peer up hdr\n",
	config.name, bms->log_str, peer->addr_str);
    return;
  }

  bmp_peer_hdr_get_peer_type(bph, &bdata.chars.peer_type);
  if (bdata.chars.peer_type == BMP_PEER_TYPE_LOC_RIB) {
    bmp_peer_hdr_get_f_flag(bph, &bdata.chars.is_filtered);
    bdata.chars.is_loc = TRUE;
  }
  else {
    bmp_peer_hdr_get_v_flag(bph, &bdata.family);
    bmp_peer_hdr_get_l_flag(bph, &bdata.chars.is_post);
    bmp_peer_hdr_get_a_flag(bph, &bdata.chars.is_2b_asn);
    bmp_peer_hdr_get_o_flag(bph, &bdata.chars.is_out);
  }

  bmp_peer_hdr_get_peer_ip(bph, &bdata.peer_ip, bdata.family);
  bmp_peer_hdr_get_bgp_id(bph, &bdata.bgp_id);
  bmp_peer_hdr_get_tstamp(bph, &bdata.tstamp);
  bmp_peer_hdr_get_peer_asn(bph, &bdata.peer_asn);

  if (bdata.family) {
    /* If no timestamp in BMP then let's generate one */
    if (!bdata.tstamp.tv_sec) gettimeofday(&bdata.tstamp, NULL);

    {
      struct bmp_log_peer_up blpu;
      struct bgp_peer bgp_peer_loc, bgp_peer_rem, *bmpp_bgp_peer;
      struct bmp_chars bmed_bmp;
      struct bgp_msg_data bmd;
      int bgp_open_len;
      void *ret;

      /* TLV vars */
      struct bmp_tlv_hdr *bth; 
      u_int16_t bmp_tlv_type, bmp_tlv_len;
      char *bmp_tlv_value;

      memset(&bgp_peer_loc, 0, sizeof(bgp_peer_loc));
      memset(&bgp_peer_rem, 0, sizeof(bgp_peer_rem));
      memset(&bmd, 0, sizeof(bmd));
      memset(&bmed_bmp, 0, sizeof(bmed_bmp));
      blpu.tlv.entries = 0;

      bmp_peer_up_hdr_get_loc_port(bpuh, &blpu.loc_port);
      bmp_peer_up_hdr_get_rem_port(bpuh, &blpu.rem_port);
      bmp_peer_up_hdr_get_local_ip(bpuh, &blpu.local_ip, bdata.family);

      bmd.peer = &bgp_peer_loc;
      bmd.extra.id = BGP_MSG_EXTRA_DATA_BMP;
      bmd.extra.len = sizeof(bmed_bmp);
      bmd.extra.data = &bmed_bmp;
      bgp_msg_data_set_data_bmp(&bmed_bmp, &bdata);

      /* XXX: checks, ie. marker, message length, etc., bypassed */
      bgp_open_len = bgp_parse_open_msg(&bmd, (*bmp_packet), FALSE, FALSE);
      bmp_get_and_check_length(bmp_packet, len, bgp_open_len);
      memcpy(&bgp_peer_loc.addr, &blpu.local_ip, sizeof(struct host_addr));

      bmd.peer = &bgp_peer_rem;
      bgp_open_len = bgp_parse_open_msg(&bmd, (*bmp_packet), FALSE, FALSE);
      bmp_get_and_check_length(bmp_packet, len, bgp_open_len);
      memcpy(&bgp_peer_rem.addr, &bdata.peer_ip, sizeof(struct host_addr));

      bmpp_bgp_peer = bmp_sync_loc_rem_peers(&bgp_peer_loc, &bgp_peer_rem);
      bmpp_bgp_peer->log = bmpp->self.log; 
      bmpp_bgp_peer->bmp_se = bmpp; /* using bmp_se field to back-point a BGP peer to its parent BMP peer */  
      ret = pm_tsearch(bmpp_bgp_peer, &bmpp->bgp_peers, bgp_peer_cmp, sizeof(struct bgp_peer));
      if (!ret) Log(LOG_WARNING, "WARN ( %s/%s ): [%s] [peer up] tsearch() unable to insert.\n", config.name, bms->log_str, peer->addr_str);

      while ((*len)) {
	if (!(bth = (struct bmp_tlv_hdr *) bmp_get_and_check_length(bmp_packet, len, sizeof(struct bmp_tlv_hdr)))) {
	  Log(LOG_INFO, "INFO ( %s/%s ): [%s] [peer up] packet discarded: failed bmp_get_and_check_length() BMP TLV hdr\n",
		config.name, bms->log_str, peer->addr_str);
	  return;
	}

	bmp_tlv_hdr_get_type(bth, &bmp_tlv_type);
	bmp_tlv_hdr_get_len(bth, &bmp_tlv_len);

	if (!(bmp_tlv_value = bmp_get_and_check_length(bmp_packet, len, bmp_tlv_len))) {
	  Log(LOG_INFO, "INFO ( %s/%s ): [%s] [peer up] packet discarded: failed bmp_get_and_check_length() BMP TLV info\n",
		config.name, bms->log_str, peer->addr_str);
	  return;
	}

	blpu.tlv.e[blpu.tlv.entries].type = bmp_tlv_type;
	blpu.tlv.e[blpu.tlv.entries].len = bmp_tlv_len;
	blpu.tlv.e[blpu.tlv.entries].val = bmp_tlv_value;

        blpu.tlv.entries = bmp_tlv_array_increment(blpu.tlv.entries, BMP_PEER_UP_INFO_ENTRIES);
      }

      if (bms->msglog_backend_methods) {
        char event_type[] = "log";

        bmp_log_msg(peer, &bdata, &blpu, bgp_peer_log_seq_get(&bms->log_seq), event_type, config.nfacctd_bmp_msglog_output, BMP_LOG_TYPE_PEER_UP);
      }

      if (bms->dump_backend_methods)
        bmp_dump_se_ll_append(peer, &bdata, &blpu, BMP_LOG_TYPE_PEER_UP);

      if (bms->msglog_backend_methods || bms->dump_backend_methods)
        bgp_peer_log_seq_increment(&bms->log_seq);
    }
  }
}

void bmp_process_msg_peer_down(char **bmp_packet, u_int32_t *len, struct bmp_peer *bmpp)
{
  struct bgp_misc_structs *bms;
  struct bgp_peer *peer, *bmpp_bgp_peer;
  struct bmp_data bdata;
  struct bmp_peer_hdr *bph;
  struct bmp_peer_down_hdr *bpdh;
  void *ret;

  if (!bmpp) return;

  peer = &bmpp->self;
  bms = bgp_select_misc_db(peer->type);

  if (!bms) return;

  memset(&bdata, 0, sizeof(bdata));

  if (!(bph = (struct bmp_peer_hdr *) bmp_get_and_check_length(bmp_packet, len, sizeof(struct bmp_peer_hdr)))) {
    Log(LOG_INFO, "INFO ( %s/%s ): [%s] [peer down] packet discarded: failed bmp_get_and_check_length() BMP peer hdr\n",
        config.name, bms->log_str, peer->addr_str);
    return;
  }

  if (!(bpdh = (struct bmp_peer_down_hdr *) bmp_get_and_check_length(bmp_packet, len, sizeof(struct bmp_peer_down_hdr)))) {
    Log(LOG_INFO, "INFO ( %s/%s ): [%s] [peer down] packet discarded: failed bmp_get_and_check_length() BMP peer down hdr\n",
        config.name, bms->log_str, peer->addr_str);
    return;
  }

  bmp_peer_hdr_get_peer_type(bph, &bdata.chars.peer_type);
  if (bdata.chars.peer_type == BMP_PEER_TYPE_LOC_RIB) {
    bdata.chars.is_loc = TRUE;
  }
  else {
    bmp_peer_hdr_get_v_flag(bph, &bdata.family);
    bmp_peer_hdr_get_l_flag(bph, &bdata.chars.is_post);
    bmp_peer_hdr_get_o_flag(bph, &bdata.chars.is_out);
  }

  bmp_peer_hdr_get_peer_ip(bph, &bdata.peer_ip, bdata.family);
  bmp_peer_hdr_get_bgp_id(bph, &bdata.bgp_id);
  bmp_peer_hdr_get_tstamp(bph, &bdata.tstamp);
  bmp_peer_hdr_get_peer_asn(bph, &bdata.peer_asn);

  if (bdata.family) {
    /* If no timestamp in BMP then let's generate one */
    if (!bdata.tstamp.tv_sec) gettimeofday(&bdata.tstamp, NULL);

    {
      struct bmp_log_peer_down blpd;

      bmp_peer_down_hdr_get_reason(bpdh, &blpd.reason);
      if (blpd.reason == BMP_PEER_DOWN_LOC_CODE) bmp_peer_down_hdr_get_loc_code(bmp_packet, len, &blpd.loc_code);

      if (bms->msglog_backend_methods) {
        char event_type[] = "log";

        bmp_log_msg(peer, &bdata, &blpd, bgp_peer_log_seq_get(&bms->log_seq), event_type, config.nfacctd_bmp_msglog_output, BMP_LOG_TYPE_PEER_DOWN);
      }

      if (bms->dump_backend_methods)
        bmp_dump_se_ll_append(peer, &bdata, &blpd, BMP_LOG_TYPE_PEER_DOWN);

      if (bms->msglog_backend_methods || bms->dump_backend_methods)
        bgp_peer_log_seq_increment(&bms->log_seq);
    }

    ret = pm_tfind(&bdata.peer_ip, &bmpp->bgp_peers, bgp_peer_host_addr_cmp);

    if (ret) {
      char peer_str[] = "peer_ip", *saved_peer_str = bms->peer_str;

      bmpp_bgp_peer = (*(struct bgp_peer **) ret);
    
      bms->peer_str = peer_str;
      bgp_peer_info_delete(bmpp_bgp_peer);
      bms->peer_str = saved_peer_str;

      pm_tdelete(&bdata.peer_ip, &bmpp->bgp_peers, bgp_peer_host_addr_cmp);
    } 
    /* missing BMP peer up message, ie. case of replay/replication of BMP messages */
    else {
      char peer_ip[INET6_ADDRSTRLEN];

      addr_to_str(peer_ip, &bdata.peer_ip);

      if (!log_notification_isset(&bmpp->missing_peer_up, bdata.tstamp.tv_sec)) {
        log_notification_set(&bmpp->missing_peer_up, bdata.tstamp.tv_sec, BMP_MISSING_PEER_UP_LOG_TOUT);
        Log(LOG_INFO, "INFO ( %s/%s ): [%s] [peer down] packet discarded: missing peer up BMP message for peer %s\n",
                config.name, bms->log_str, peer->addr_str, peer_ip);
      }
    }
  }
}

void bmp_process_msg_route_monitor(char **bmp_packet, u_int32_t *len, struct bmp_peer *bmpp)
{
  struct bgp_misc_structs *bms;
  struct bgp_peer *peer, *bmpp_bgp_peer;
  struct bmp_data bdata;
  struct bmp_peer_hdr *bph;
  char tstamp_str[SRVBUFLEN], peer_ip[INET6_ADDRSTRLEN];
  int bgp_update_len;
  void *ret;

  if (!bmpp) return;

  peer = &bmpp->self;
  bms = bgp_select_misc_db(peer->type);

  if (!bms) return;

  memset(&bdata, 0, sizeof(bdata));

  if (!(bph = (struct bmp_peer_hdr *) bmp_get_and_check_length(bmp_packet, len, sizeof(struct bmp_peer_hdr)))) {
    Log(LOG_INFO, "INFO ( %s/%s ): [%s] [route] packet discarded: failed bmp_get_and_check_length() BMP peer hdr\n",
        config.name, bms->log_str, peer->addr_str);
    return;
  }

  bmp_peer_hdr_get_peer_type(bph, &bdata.chars.peer_type);
  if (bdata.chars.peer_type == BMP_PEER_TYPE_LOC_RIB) {
    bmp_peer_hdr_get_f_flag(bph, &bdata.chars.is_filtered);
    bdata.chars.is_loc = TRUE;
  }
  else {
    bmp_peer_hdr_get_v_flag(bph, &bdata.family);
    bmp_peer_hdr_get_l_flag(bph, &bdata.chars.is_post);
    bmp_peer_hdr_get_a_flag(bph, &bdata.chars.is_2b_asn);
    bmp_peer_hdr_get_o_flag(bph, &bdata.chars.is_out);
  }

  bmp_peer_hdr_get_peer_ip(bph, &bdata.peer_ip, bdata.family);
  bmp_peer_hdr_get_bgp_id(bph, &bdata.bgp_id);
  bmp_peer_hdr_get_tstamp(bph, &bdata.tstamp);
  bmp_peer_hdr_get_peer_asn(bph, &bdata.peer_asn);

  if (bdata.family) {
    /* If no timestamp in BMP then let's generate one */
    if (!bdata.tstamp.tv_sec) gettimeofday(&bdata.tstamp, NULL);

    compose_timestamp(tstamp_str, SRVBUFLEN, &bdata.tstamp, TRUE,
		      config.timestamps_since_epoch, config.timestamps_rfc3339,
		      config.timestamps_utc);
    addr_to_str(peer_ip, &bdata.peer_ip);

    ret = pm_tfind(&bdata.peer_ip, &bmpp->bgp_peers, bgp_peer_host_addr_cmp);

    if (ret) {
      char peer_str[] = "peer_ip", *saved_peer_str = bms->peer_str;
      char peer_port_str[] = "peer_tcp_port", *saved_peer_port_str = bms->peer_port_str;
      struct bmp_chars bmed_bmp;
      struct bgp_msg_data bmd;

      bmpp_bgp_peer = (*(struct bgp_peer **) ret);
      memset(&bmd, 0, sizeof(bmd));
      memset(&bmed_bmp, 0, sizeof(bmed_bmp));

      bms->peer_str = peer_str;
      bms->peer_port_str = peer_port_str;
      bmd.peer = bmpp_bgp_peer;
      bmd.extra.id = BGP_MSG_EXTRA_DATA_BMP;
      bmd.extra.len = sizeof(bmed_bmp);
      bmd.extra.data = &bmed_bmp;
      bgp_msg_data_set_data_bmp(&bmed_bmp, &bdata);

      /* XXX: checks, ie. marker, message length, etc., bypassed */
      bgp_update_len = bgp_parse_update_msg(&bmd, (*bmp_packet)); 
      bms->peer_str = saved_peer_str;
      bms->peer_port_str = saved_peer_port_str;

      bmp_get_and_check_length(bmp_packet, len, bgp_update_len);
    }
    /* missing BMP peer up message, ie. case of replay/replication of BMP messages */
    else {
      if (!log_notification_isset(&bmpp->missing_peer_up, bdata.tstamp.tv_sec)) {
	log_notification_set(&bmpp->missing_peer_up, bdata.tstamp.tv_sec, BMP_MISSING_PEER_UP_LOG_TOUT);
	Log(LOG_INFO, "INFO ( %s/%s ): [%s] [route] packet discarded: missing peer up BMP message for peer %s\n",
		config.name, bms->log_str, peer->addr_str, peer_ip);
      }
    }
  }
}

void bmp_process_msg_route_mirror(char **bmp_packet, u_int32_t *len, struct bmp_peer *bmpp)
{
  // XXX: maybe support route mirroring
}

void bmp_process_msg_stats(char **bmp_packet, u_int32_t *len, struct bmp_peer *bmpp)
{
  struct bgp_misc_structs *bms;
  struct bgp_peer *peer;
  struct bmp_data bdata;
  struct bmp_peer_hdr *bph;
  struct bmp_stats_hdr *bsh;
  struct bmp_stats_cnt_hdr *bsch;
  u_int64_t cnt_data64;
  u_int32_t index, count = 0, cnt_data32;
  u_int16_t cnt_type, cnt_len;
  u_int8_t got_data;
  afi_t afi;
  safi_t safi;

  if (!bmpp) return;

  peer = &bmpp->self;
  bms = bgp_select_misc_db(peer->type);

  if (!bms) return;

  memset(&bdata, 0, sizeof(bdata));

  if (!(bph = (struct bmp_peer_hdr *) bmp_get_and_check_length(bmp_packet, len, sizeof(struct bmp_peer_hdr)))) {
    Log(LOG_INFO, "INFO ( %s/%s ): [%s] [stats] packet discarded: failed bmp_get_and_check_length() BMP peer hdr\n",
        config.name, bms->log_str, peer->addr_str);
    return;
  }

  if (!(bsh = (struct bmp_stats_hdr *) bmp_get_and_check_length(bmp_packet, len, sizeof(struct bmp_stats_hdr)))) {
    Log(LOG_INFO, "INFO ( %s/%s ): [%s] [stats] packet discarded: failed bmp_get_and_check_length() BMP stats hdr\n",
        config.name, bms->log_str, peer->addr_str);
    return;
  }

  bmp_peer_hdr_get_peer_type(bph, &bdata.chars.peer_type);
  if (bdata.chars.peer_type == BMP_PEER_TYPE_LOC_RIB) {
    bmp_peer_hdr_get_f_flag(bph, &bdata.chars.is_filtered);
    bdata.chars.is_loc = TRUE;
  }
  else {
    bmp_peer_hdr_get_v_flag(bph, &bdata.family);
    bmp_peer_hdr_get_l_flag(bph, &bdata.chars.is_post);
    bmp_peer_hdr_get_o_flag(bph, &bdata.chars.is_out);
  }

  bmp_peer_hdr_get_peer_ip(bph, &bdata.peer_ip, bdata.family);
  bmp_peer_hdr_get_bgp_id(bph, &bdata.bgp_id);
  bmp_peer_hdr_get_tstamp(bph, &bdata.tstamp);
  bmp_peer_hdr_get_peer_asn(bph, &bdata.peer_asn);
  bmp_stats_hdr_get_count(bsh, &count);

  if (bdata.family) {
    /* If no timestamp in BMP then let's generate one */
    if (!bdata.tstamp.tv_sec) gettimeofday(&bdata.tstamp, NULL);

    for (index = 0; index < count; index++) {
      if (!(bsch = (struct bmp_stats_cnt_hdr *) bmp_get_and_check_length(bmp_packet, len, sizeof(struct bmp_stats_cnt_hdr)))) {
        Log(LOG_INFO, "INFO ( %s/%s ): [%s] [stats] packet discarded: failed bmp_get_and_check_length() BMP stats cnt hdr #%u\n",
		config.name, bms->log_str, peer->addr_str, index);
        return;
      }

      bmp_stats_cnt_hdr_get_type(bsch, &cnt_type);
      bmp_stats_cnt_hdr_get_len(bsch, &cnt_len);
      cnt_data32 = 0, cnt_data64 = 0, afi = 0, safi = 0, got_data = TRUE;

      switch (cnt_type) {
      case BMP_STATS_TYPE0:
        if (cnt_len == 4) bmp_stats_cnt_get_data32(bmp_packet, len, &cnt_data32);
        break;
      case BMP_STATS_TYPE1:
        if (cnt_len == 4) bmp_stats_cnt_get_data32(bmp_packet, len, &cnt_data32);
        break;
      case BMP_STATS_TYPE2:
        if (cnt_len == 4) bmp_stats_cnt_get_data32(bmp_packet, len, &cnt_data32);
        break;
      case BMP_STATS_TYPE3:
        if (cnt_len == 4) bmp_stats_cnt_get_data32(bmp_packet, len, &cnt_data32);
        break;
      case BMP_STATS_TYPE4:
        if (cnt_len == 4) bmp_stats_cnt_get_data32(bmp_packet, len, &cnt_data32);
        break;
      case BMP_STATS_TYPE5:
        if (cnt_len == 4) bmp_stats_cnt_get_data32(bmp_packet, len, &cnt_data32);
        break;
      case BMP_STATS_TYPE6:
        if (cnt_len == 4) bmp_stats_cnt_get_data32(bmp_packet, len, &cnt_data32);
        break;
      case BMP_STATS_TYPE7:
        if (cnt_len == 8) bmp_stats_cnt_get_data64(bmp_packet, len, &cnt_data64);
        break;
      case BMP_STATS_TYPE8:
        if (cnt_len == 8) bmp_stats_cnt_get_data64(bmp_packet, len, &cnt_data64);
        break;
      case BMP_STATS_TYPE9:
	if (cnt_len == 11) bmp_stats_cnt_get_afi_safi_data64(bmp_packet, len, &afi, &safi, &cnt_data64);
	break;
      case BMP_STATS_TYPE10:
	if (cnt_len == 11) bmp_stats_cnt_get_afi_safi_data64(bmp_packet, len, &afi, &safi, &cnt_data64);
	break;
      case BMP_STATS_TYPE11:
        if (cnt_len == 4) bmp_stats_cnt_get_data32(bmp_packet, len, &cnt_data32);
        break;
      case BMP_STATS_TYPE12:
        if (cnt_len == 4) bmp_stats_cnt_get_data32(bmp_packet, len, &cnt_data32);
        break;
      case BMP_STATS_TYPE13:
        if (cnt_len == 4) bmp_stats_cnt_get_data32(bmp_packet, len, &cnt_data32);
        break;
      case BMP_STATS_TYPE14:
	if (cnt_len == 8) bmp_stats_cnt_get_data64(bmp_packet, len, &cnt_data64);
	break;
      case BMP_STATS_TYPE15:
	if (cnt_len == 8) bmp_stats_cnt_get_data64(bmp_packet, len, &cnt_data64);
	break;
      case BMP_STATS_TYPE16:
	if (cnt_len == 11) bmp_stats_cnt_get_afi_safi_data64(bmp_packet, len, &afi, &safi, &cnt_data64);
	break;
      case BMP_STATS_TYPE17:
	if (cnt_len == 11) bmp_stats_cnt_get_afi_safi_data64(bmp_packet, len, &afi, &safi, &cnt_data64);
	break;
      default:
        if (cnt_len == 4) bmp_stats_cnt_get_data32(bmp_packet, len, &cnt_data32);
        else if (cnt_len == 8) bmp_stats_cnt_get_data64(bmp_packet, len, &cnt_data64);
        else {
          bmp_get_and_check_length(bmp_packet, len, cnt_len);
          got_data = FALSE;
        }
        break;
      }

      if (cnt_data32 && !cnt_data64) cnt_data64 = cnt_data32; 

      { 
        struct bmp_log_stats blstats;

        blstats.cnt_type = cnt_type;
	blstats.cnt_afi = afi;
	blstats.cnt_safi = safi;
        blstats.cnt_data = cnt_data64;
        blstats.got_data = got_data;

        if (bms->msglog_backend_methods) {
          char event_type[] = "log";

          bmp_log_msg(peer, &bdata, &blstats, bgp_peer_log_seq_get(&bms->log_seq), event_type, config.nfacctd_bmp_msglog_output, BMP_LOG_TYPE_STATS);
        } 

        if (bms->dump_backend_methods)
          bmp_dump_se_ll_append(peer, &bdata, &blstats, BMP_LOG_TYPE_STATS);

        if (bms->msglog_backend_methods || bms->dump_backend_methods)
          bgp_peer_log_seq_increment(&bms->log_seq);
      }
    }
  }
}

void bmp_common_hdr_get_len(struct bmp_common_hdr *bch, u_int32_t *len)
{
  if (bch && len) (*len) = ntohl(bch->len);
}

void bmp_tlv_hdr_get_type(struct bmp_tlv_hdr *bth, u_int16_t *type)
{
  if (bth && type) (*type) = ntohs(bth->type);
}

void bmp_tlv_hdr_get_len(struct bmp_tlv_hdr *bth, u_int16_t *len)
{
  if (bth && len) (*len) = ntohs(bth->len);
}

void bmp_term_hdr_get_reason_type(char **bmp_packet, u_int32_t *pkt_size, u_int16_t *type)
{
  char *ptr;

  if (bmp_packet && (*bmp_packet) && pkt_size && type) {
    ptr = bmp_get_and_check_length(bmp_packet, pkt_size, 2);
    memcpy(type, ptr, 2);
    (*type) = ntohs((*type));
  }
}

void bmp_peer_hdr_get_v_flag(struct bmp_peer_hdr *bph, u_int8_t *family)
{
  u_int8_t version;

  if (bph && family) {
    version = (bph->flags & BMP_PEER_FLAGS_ARI_V);
    (*family) = FALSE;

    if (version == 0) (*family) = AF_INET;
    else if (version == 1) (*family) = AF_INET6;
  }
}

void bmp_peer_hdr_get_l_flag(struct bmp_peer_hdr *bph, u_int8_t *is_post)
{
  if (bph && is_post) {
    if (bph->flags & BMP_PEER_FLAGS_ARI_L) (*is_post) = TRUE;
    else (*is_post) = FALSE;
  }
}

void bmp_peer_hdr_get_a_flag(struct bmp_peer_hdr *bph, u_int8_t *is_2b_asn)
{
  if (bph && is_2b_asn) {
    if (bph->flags & BMP_PEER_FLAGS_ARI_A) (*is_2b_asn) = TRUE;
    else (*is_2b_asn) = FALSE;
  }
}

void bmp_peer_hdr_get_f_flag(struct bmp_peer_hdr *bph, u_int8_t *is_filtered)
{
  if (bph && is_filtered) {
    if (bph->flags & BMP_PEER_FLAGS_LR_F) (*is_filtered) = TRUE;
    else (*is_filtered) = FALSE;
  }
}

void bmp_peer_hdr_get_o_flag(struct bmp_peer_hdr *bph, u_int8_t *is_out)
{
  if (bph && is_out) {
    if (bph->flags & BMP_PEER_FLAGS_ARO_O)  (*is_out) = TRUE;
    else (*is_out) = FALSE;
  }
}

void bmp_peer_hdr_get_peer_ip(struct bmp_peer_hdr *bph, struct host_addr *a, u_int8_t family)
{
  if (bph && a) {
    a->family = family;

    if (family == AF_INET) a->address.ipv4.s_addr = bph->addr[3]; 
    else if (family == AF_INET6) memcpy(&a->address.ipv6, &bph->addr, 16); 
    else memset(a, 0, sizeof(struct host_addr));
  }
}

void bmp_peer_hdr_get_bgp_id(struct bmp_peer_hdr *bph, struct host_addr *a)
{
  if (bph && a) {
    a->family = AF_INET;
    a->address.ipv4.s_addr = bph->bgp_id;
  }
}

void bmp_peer_hdr_get_tstamp(struct bmp_peer_hdr *bph, struct timeval *tv)
{
  u_int32_t sec, usec;

  if (bph && tv) {
    if (bph->tstamp_sec) {
      sec = ntohl(bph->tstamp_sec);
      usec = ntohl(bph->tstamp_usec);

      tv->tv_sec = sec;
      tv->tv_usec = usec;
    }
  }
}

void bmp_peer_hdr_get_peer_asn(struct bmp_peer_hdr *bph, u_int32_t *asn)
{
  if (bph && asn) (*asn) = ntohl(bph->asn);
}

void bmp_peer_hdr_get_peer_type(struct bmp_peer_hdr *bph, u_int8_t *type)
{
  if (bph && type) (*type) = bph->type;
}

void bmp_peer_up_hdr_get_local_ip(struct bmp_peer_up_hdr *bpuh, struct host_addr *a, u_int8_t family)
{
  if (bpuh && a && family) {
    a->family = family;

    if (family == AF_INET) a->address.ipv4.s_addr = bpuh->loc_addr[3];
    else if (family == AF_INET6) memcpy(&a->address.ipv6, &bpuh->loc_addr, 16);
  }
}

void bmp_peer_up_hdr_get_loc_port(struct bmp_peer_up_hdr *bpuh, u_int16_t *port)
{
  if (bpuh && port) (*port) = ntohs(bpuh->loc_port);
}

void bmp_peer_up_hdr_get_rem_port(struct bmp_peer_up_hdr *bpuh, u_int16_t *port)
{
  if (bpuh && port) (*port) = ntohs(bpuh->rem_port);
}

void bmp_peer_down_hdr_get_reason(struct bmp_peer_down_hdr *bpdh, u_char *reason)
{
  if (bpdh && reason) (*reason) = bpdh->reason;
}

void bmp_peer_down_hdr_get_loc_code(char **bmp_packet, u_int32_t *pkt_size, u_int16_t *code)
{
  char *ptr;
 
  if (bmp_packet && (*bmp_packet) && pkt_size && code) {
    ptr = bmp_get_and_check_length(bmp_packet, pkt_size, 2); 
    memcpy(code, ptr, 2);
    (*code) = ntohs((*code));
  }
}

void bmp_stats_hdr_get_count(struct bmp_stats_hdr *bsh, u_int32_t *count)
{
  if (bsh && count) (*count) = ntohl(bsh->count);
}

void bmp_stats_cnt_hdr_get_type(struct bmp_stats_cnt_hdr *bsch, u_int16_t *type)
{
  if (bsch && type) (*type) = ntohs(bsch->type);
}

void bmp_stats_cnt_hdr_get_len(struct bmp_stats_cnt_hdr *bsch, u_int16_t *len)
{
  if (bsch && len) (*len) = ntohs(bsch->len);
}

void bmp_stats_cnt_get_data32(char **bmp_packet, u_int32_t *pkt_size, u_int32_t *data)
{
  char *ptr;

  if (bmp_packet && (*bmp_packet) && pkt_size && data) {
    ptr = bmp_get_and_check_length(bmp_packet, pkt_size, 4);
    memcpy(data, ptr, 4);
    (*data) = ntohl((*data));
  }
}

void bmp_stats_cnt_get_data64(char **bmp_packet, u_int32_t *pkt_size, u_int64_t *data)
{
  char *ptr;

  if (bmp_packet && (*bmp_packet) && pkt_size && data) {
    ptr = bmp_get_and_check_length(bmp_packet, pkt_size, 8);
    memcpy(data, ptr, 8);
    (*data) = pm_ntohll((*data));
  }
}

void bmp_stats_cnt_get_afi_safi_data64(char **bmp_packet, u_int32_t *pkt_size, afi_t *afi, safi_t *safi, u_int64_t *data)
{
  char *ptr;

  if (bmp_packet && (*bmp_packet) && pkt_size && afi && safi && data) {
    ptr = bmp_get_and_check_length(bmp_packet, pkt_size, 2);
    memcpy(afi, ptr, 2);
    (*afi) = ntohs((*afi));

    ptr = bmp_get_and_check_length(bmp_packet, pkt_size, 1);
    memcpy(safi, ptr, 1);

    ptr = bmp_get_and_check_length(bmp_packet, pkt_size, 8);
    memcpy(data, ptr, 8);
    (*data) = pm_ntohll((*data));
  }
}
