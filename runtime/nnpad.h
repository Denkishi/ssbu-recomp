/* Host side of nn::hid devices: the one interface nnhid.c uses to read pads (nnpad_sdl.c implements it with SDL3).
 * Button numbers are the Pro Controller bits of nnhid.c (0 A, 1 B, 2 X, 3 Y, 4 StickL, 5 StickR, 6 L, 7 R, 8 ZL, 9 ZR,
 * 10 Plus, 11 Minus, 12 Left, 13 Up, 14 Right, 15 Down). Players are 0..7. */
#ifndef NNPAD_H
#define NNPAD_H
#include <stdint.h>

typedef struct {
    uint64_t buttons;           /* bits 0..15 only (the stick-as-button bits are derived by nnhid.c) */
    int lx, ly, rx, ry;         /* -32767..32767, up/right positive, dead zone and circular normalisation applied */
    int wired;                  /* 0 = bluetooth / wireless */
    int has_motion;             /* the device has a gyro and accelerometer */
    float accel[3];             /* in G, Nintendo axes (see nnpad_sdl.c; the frame is NOT VERIFIED against a console) */
    float gyro[3];              /* in revolutions per second, same axes */
} NnPadState;

int  nnpad_init(void);                           /* SDL up (no video, no window); 0 = failed. Also loads controllers.ini. */
void nnpad_poll(int force);                      /* hot-plug, config reload; cheap when called often (at most once per ms). With the pad thread: nothing, or (force) one full round of it, waited for */
int  nnpad_thread_stats(long long out[4]);       /* the pad thread's rounds: count, count over 1 ms, longest (us), longest start-to-start (us); 0 = no pad thread */
long long nnpad_sample_age_us(int player);       /* microseconds since the pad thread read this player's device; -1 = no device or no pad thread */
int  nnpad_present(int player);                  /* a device is assigned to this player slot */
int  nnpad_get(int player, NnPadState *out);     /* 0 = none */
void nnpad_rumble(int player, float low, float high);   /* amplitudes 0..1 of the low and high band */
void nnpad_disconnect(int player);               /* DisconnectNpad: hidden until one of its buttons is pressed */
void nnpad_swap(int a, int b);                   /* SwapNpadAssignment */
int  nnpad_keyboard_player(void);                /* 0..7, -1 = nobody; fixed once resolved (see controllers.ini) */
int  nnpad_kb_vk(int idx);                       /* configured virtual key: 0..15 = Pro buttons, 16..19 = left stick left/up/right/down, 20..23 = right stick; 0 = unbound */
int  nnpad_tool(int argc, char **argv);          /* --pad-test / --pad-map <n> / SSBU_PAD_TEST: returns the exit code, or -1 if this is no tool request */

/* self-test hooks implemented by nnhid.c (they call the handlers the game calls) */
typedef struct {
    uint32_t (*style)(uint32_t id);
    void (*state)(uint32_t id, uint64_t *buttons, int32_t stick[4], uint32_t *attr);
    void (*vibrate)(uint32_t id, float low, float high);
    int (*six)(uint32_t id, float accel[3], float gyro[3], int *at_rest);
    void (*test_mode)(int on);
    uint32_t (*led)(uint32_t id);
} NnHidHooks;
int nnpad_selftest(const NnHidHooks *h);         /* number of failed checks; only virtual devices count while it runs */
#endif
