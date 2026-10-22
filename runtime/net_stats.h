/* U1: what the stats overlay of the settings menu shows about online play (menu.cpp, F2; plan\phases\U1_settings_menu.md).
 *
 * The overlay calls net_stats_get() once per drawn frame on the render thread. menu.cpp has a default that answers
 * "Offline" (net_stats_get_default, bound with /alternatename): the online code defines net_stats_get() itself and
 * the linker takes that one instead, with no change here. It must be cheap and must not block (read a few ints).
 *
 * Who fills which field:
 *   state, players, ping_ms       the online layer (nnldn.c / net_relay.c, track N3): connection state, players in the
 *                                 session (this PC included), round-trip time to the other side in ms (-1 = unknown)
 *   delay                         the rollback code (rb_*.c, track N1/R2): input delay in frames used now
 *   rollback_last, rollback_max   the rollback code: frames rolled back at the last rollback, the most this session
 *   ping_to, direct, peers        what ping_ms measures (NET_PING_*); of the other players of the room (peers), how many
 *                                 are reached directly (direct); delay / rollback_* mean something only with NET_PING_ROLLBACK
 * Run-ahead is not here: the overlay reads it itself (graphics.ini runahead / SSBU_RUNAHEAD, r1_state.c r1_ra_phase). */
#ifndef NET_STATS_H
#define NET_STATS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { NET_OFFLINE = 0, NET_CONNECTING = 1, NET_CONNECTED = 2, NET_DISCONNECTED = 3 };
/* ping_ms is the round trip to: the lobby server (no other player, or every one of them reached through the lobby); a
 * player reached directly; the other game of an online rollback match (its own measure) */
enum { NET_PING_LOBBY = 0, NET_PING_DIRECT = 1, NET_PING_ROLLBACK = 2 };
typedef struct { int state; int players; int ping_ms; int delay; int rollback_last, rollback_max; int ping_to, direct, peers; } NetStats;
void net_stats_get(NetStats *out);          /* fills every field; the default: state NET_OFFLINE, ping -1, the rest 0 */

/* O1 (plan\phases\O1_room_browser.md): the room browser of the settings menu (F1 > Online rooms), net_relay.c.
 * Every room of this lobby code, any game version or mod set; joinable = same build id as this game, otherwise why
 * says the cause in one or more lines ("Different version: this room is 13.0.4, you have 13.0.5", "MISSING: a, b"). */
typedef struct {
    uint32_t host;                          /* the host's virtual address */
    char name[33], build[41], version[24];  /* host name, build id, game version taken from it */
    int players, most, open, mine, joinable, mods_known, nmods;
    char mods[256];                         /* the room's mods, comma separated ("" = none) */
    char why[256];                          /* joinable == 0: the cause */
} NetRoom;
/* the rooms (at most max) and the browser's state: 0 online off, 1 waiting for the lobby, 2 the lobby does not answer
 * (an older lobby), 3 listed. Calling it keeps the list fresh (asked every 2 s while called). Never blocks long. */
int net_rooms_get(NetRoom *out, int max, int *state);
const char *net_build_id(void);             /* this game's build id ("ssbu-recomp-ldn1-13.0.5[+m<hash>]") */
const char *net_mods_text(void);            /* this game's mods ("none" or "name (hash), ...") */

/* O2 (plan\phases\O2_matchmaking.md): "Find an opponent" (F1 > Online), net_relay.c. The lobby pairs two searching players
 * of the same build id and lobby code and says who hosts; the host creates the Local Wireless room, the other finds it
 * first in the game's Rooms List. */
enum { MM_IDLE = 0, MM_SEARCHING, MM_MATCHED, MM_TIMEOUT, MM_PEER_LEFT, MM_TOGETHER, MM_EXPIRED,   /* (the lobby's states) */
       MM_ASKING, MM_OLD_LOBBY, MM_OFFLINE };                               /* asked, no answer yet; an older lobby; online off / not joined */
enum { MM_ROLE_HOST = 1, MM_ROLE_JOIN = 2 };
typedef struct {
    int state, role, room_open;             /* MM_*, MM_ROLE_* (MATCHED / TOGETHER), the host's room is open (listed) */
    uint32_t peer;                          /* the opponent's virtual address */
    char peer_name[33];
    int waited_s;                           /* seconds searching, or since the match */
    int players, searching, code_players;   /* lobby counts: players of this build and code, of them searching, players of
                                               every build of this code; -1 = not known (an older lobby / not joined yet) */
    int kind;                               /* (R3 net3) 1 = a Local Wireless search (F1 > Online), 2 = a rollback-match search (rb_net.c) */
} NetMatch;
void net_match_find(void);                  /* start searching (again) */
void net_match_cancel(void);                /* stop searching, or leave the match (the other player is told) */
void net_match_get(NetMatch *out);          /* never blocks */
/* (R3 net3, lobby 1.4) a search for a ROLLBACK match (rb_net.c: Smash versus with rollback on, in lobby / LAN mode, outside
 * a Local Wireless room): paired only with another such search; MM_MATCHED / MM_TOGETHER = the opponent is known, its
 * address is where the rollback messages go (net_rb_send) until the next search. */
void net_match_find_rb(void);
#ifdef __cplusplus
}
#endif
#endif
