/*
 * 版权属于onps栈开发团队，遵循Apache License 2.0开源许可协议
 *
 * open net protocol statck
 *
 * Neo-T, 创建于2022.03.21 10:19
 *
 * version 1.0.0.221017_RC
 */
#ifndef ONPS_H
#define ONPS_H

#ifdef SYMBOL_GLOBALS
	#define ONPS_EXT
#else
	#define ONPS_EXT extern
#endif //* SYMBOL_GLOBALS
#include "port/datatype.hh"
#include "port/sys_config.hh"
#include "onps_errors.hh"
#include "port/os_datatype.hh"
#include "port/os_adapter.hh"
#include "mmu/buddy.hh"
#include "mmu/buf_list.hh"
#include "onps_utils.hh"
#include "one_shot_timer.hh"
#include "netif/netif.hh" 
#include "netif/route.hh"
#if SUPPORT_PPP
#include "ppp/negotiation.hh"
#include "ppp/ppp.hh"
#endif
#if SUPPORT_ETHERNET
#include "ethernet/ethernet.hh"
#endif
#include "ip/icmp.hh"
#include "onps_input.hh"
#include "bsd/socket.hh"

ONPS_EXT BOOL open_npstack_load(EN_ONPSERR *penErr); 
ONPS_EXT void open_npstack_unload(void);

#endif
