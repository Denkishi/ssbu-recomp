/* Online play is not in the macOS build yet (net_relay.c is built on Windows and Linux; tools/lifter/build.py leaves it
 * out on macOS, or when the tree has no net_relay.c). These are the net_relay.h functions nnldn.c calls, answering as with SSBU_ONLINE unset:
 * net_online() is 0, so nnldn_register() registers nothing and the game's Local Wireless stays the offline console's. */
#include <stdint.h>
#include <string.h>
#include "net_relay.h"

int net_online(void) { return 0; }
int net_mode(void) { return 0; }
int net_log_level(void) { return 0; }
void net_start(void) {}
uint32_t net_my_ip(void) { return 0; }
int net_rb_no_destination(void) { return 0; }      /* (R3 pub1) rb_net.c's notice */
int net_rb_ready(void) { return 0; }                /* rb_net.c (rollback over the network: off without online play) */
int net_rb_send(const void *p, int n) { (void)p; (void)n; return 0; }
int net_lobby_gone(void) { return 0; }
void net_list_rooms(uint32_t seq) { (void)seq; }
void net_send(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const void *data, uint32_t len) {
    (void)src; (void)sport; (void)dst; (void)dport; (void)data; (void)len;
}
int net_room_refusal(uint32_t host, char *why, size_t n) { (void)host; (void)why; (void)n; return 0; }
/* the settings menu (menu.cpp: the Online pages, the stats overlay): what the online code answers when it is off */
const char *net_room_code(void) { return ""; }
const char *net_lan_code(void) { return ""; }
const char *net_upnp_result(void) { return ""; }
#include "net_stats.h"
void net_stats_get(NetStats *o) { memset(o, 0, sizeof *o); o->state = NET_OFFLINE; o->ping_ms = -1; }
int net_rooms_get(NetRoom *out, int max, int *state) { (void)out; (void)max; if (state) *state = 0; return 0; }
const char *net_build_id(void) { return ""; }
const char *net_mods_text(void) { return "none"; }
uint32_t net_match_host(void) { return 0; }
void net_match_find(void) {}
void net_match_find_rb(void) {}
void net_match_cancel(void) {}
void net_match_get(NetMatch *o) { memset(o, 0, sizeof *o); o->state = MM_OFFLINE; o->players = o->searching = o->code_players = -1; }
