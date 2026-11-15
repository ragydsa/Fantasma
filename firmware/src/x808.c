/* SPDX-License-Identifier: GPL-3.0-only */
/* The X808 kit of the DRUM engine (eng_drum.c KIT X808): X0X's TR-808 (github.com/charlesvestal/fm1-x0x,
 * firmware/src/dsp/drum808.c, GPL-3.0: the 8W8 engine by athousanddetails and Gustavo Lima, the rim shot from
 * Yoshinosuke Horiuchi's sc808, MIT), unchanged in x808/, behind an integer interface.
 *
 * The 808 is float code and the rest of this firmware is integer only, so nothing but this file sees a float:
 * on the device it is a unit of its own, compiled for the AC79's single-precision FPU (tools/build.py X808_FLAGS,
 * X0X's flags) and linked in; the host tests include it (eng_drum.c). The state lives in the part's engine memory
 * behind the DRUM lanes (x808_t, X808_BYTES of it; all zeros = not set up yet).
 *
 * One 808 per part. The DRUM engine renders a lane per voice, so a lane here is the 808 voices that share it
 * (x808_run adds them up): KICK BD, SNARE SD, CLAP CP, HATCL CH, HATOP OH, TOM the three toms / congas, RIM the
 * rim shot / claves, BELL the cowbell and the cymbal. The metal bank the cowbell, hats and cymbal share runs once a
 * block (x808_block), before the voices. */
#include <stdint.h>
#include "x808/ns_begin.h"            /* drum808.c's short names behind d8x_ (the host tests' one unit) */
#include "x808/drum808.c"

enum { X8_KICK, X8_SNARE, X8_CLAP, X8_HATC, X8_HATO, X8_TOM, X8_RIM, X8_BELL, X8_NLANE };   /* = DV_KICK .. */
#define X808_MAGIC 0x58383038u
#define X808_BLOCK 32                 /* CTL (dsp.c) = the 808's own chunk */

typedef struct {
    uint32_t magic;
    uint8_t knob[4];                  /* TUNE TONE DECY SNAP as applied (64 = the 808's defaults) */
    uint8_t bank_on, pad[3];          /* this block's bus / pair are there */
    float bus[X808_BLOCK], pair[X808_BLOCK];
    drum808_t d;
} x808_t;

_Static_assert(sizeof(x808_t) <= 4096u, "x808_t over X808_BYTES (eng_drum.c)");

static void x808_ready(x808_t *x)
{
    if (x->magic == X808_MAGIC)
        return;
    drum808_init(&x->d);
    x->knob[0] = x->knob[1] = x->knob[2] = x->knob[3] = 64;
    x->bank_on = 0;
    x->magic = X808_MAGIC;
}

static void x808_pot(x808_t *x, int s, int slot, int delta)
{
    int v = (int)k_spec[s][slot].def + delta;
    set_pot(&x->d, s, slot, v < 0 ? 0 : v > 127 ? 127 : v);
}

/* the engine's knobs (0..127, 64 = as designed) move every sound's pot from its default */
void x808_knobs(void *st, int tune, int tone, int decay, int snap)
{
    x808_t *x = (x808_t *)st;
    int s;
    x808_ready(x);
    if (tune != x->knob[0]) {
        for (s = 0; s < D8S_NUM; s++)
            x808_pot(x, s, D8P_TUNE, tune - 64);
        x->knob[0] = (uint8_t)tune;
    }
    if (tone != x->knob[1]) {
        x808_pot(x, D8S_BD, D8P_X1, tone - 64);
        x808_pot(x, D8S_SD, D8P_X2, tone - 64);
        x->knob[1] = (uint8_t)tone;
    }
    if (decay != x->knob[2]) {
        for (s = 0; s < D8S_NUM; s++)
            x808_pot(x, s, D8P_DECAY, decay - 64);
        x->knob[2] = (uint8_t)decay;
    }
    if (snap != x->knob[3]) {
        x808_pot(x, D8S_SD, D8P_X1, snap - 64);
        x808_pot(x, D8S_BD, D8P_X2, snap - 64);
        x808_pot(x, D8S_MA, D8P_X1, snap - 64);
        x->knob[3] = (uint8_t)snap;
    }
}

/* strike an 808 track (D8_*); sw: its switch (conga, claves); acc 0..65536: from a plain hit to the accent */
void x808_trigger(void *st, int track, int sw, int acc)
{
    x808_t *x = (x808_t *)st;
    x808_ready(x);
    if (track < 0 || track >= D8_NUM)
        return;
    if (track == D8_LT || track == D8_MT || track == D8_HT || track == D8_RS || track == D8_CP)
        x->d.sw[track] = (uint8_t)(sw & 1);
    if (acc < 0) acc = 0;
    if (acc > 65536) acc = 65536;
    drum808_trigger(&x->d, track, D8_VEL_NORMAL + (1.0f - D8_VEL_NORMAL) * ((float)acc * (1.0f / 65536.0f)));
}

static const uint8_t X8_LANES[X8_NLANE][3] = {     /* the 808's lanes (L_*) of each DRUM lane, L_NUM = none */
    {L_BD, L_NUM, L_NUM}, {L_SD, L_NUM, L_NUM}, {L_CP, L_MA, L_NUM}, {L_CH, L_NUM, L_NUM},
    {L_OH, L_NUM, L_NUM}, {L_T0, L_T1, L_T2}, {L_RS, L_CL, L_NUM}, {L_CB, L_CY, L_NUM},
};

int x808_live(const void *st, int lane)
{
    const x808_t *x = (const x808_t *)st;
    int k;
    if (x->magic != X808_MAGIC || lane < 0 || lane >= X8_NLANE)
        return 0;
    for (k = 0; k < 3; k++)
        if (X8_LANES[lane][k] < L_NUM && lane_on(&x->d, X8_LANES[lane][k]))
            return 1;
    return 0;
}

/* once a block, before the lanes: the shared metal bank, while a voice that hears it sounds */
void x808_block(void *st)
{
    x808_t *x = (x808_t *)st;
    if (x->magic != X808_MAGIC)
        return;
    x->bank_on = 0;
    if (lane_on(&x->d, L_CB) || lane_on(&x->d, L_CH) || lane_on(&x->d, L_OH) || lane_on(&x->d, L_CY)) {
        bank_block(&x->d.bank, x->bus, x->pair, X808_BLOCK);
        x->bank_on = 1;
    }
}

/* 8W8 balances its kit flat; here the kick leads and the open hat and clap sit back, as on the other kits (each
 * lane's LEVEL is still the player's) */
static const float X8_TRIM[X8_NLANE] = {2.0f, 1.0f, 0.7f, 1.0f, 0.6f, 1.0f, 1.0f, 1.0f};

/* a DRUM lane's next n samples (n <= 32) into y, about the level of the other kits' lanes (Q15 at full) */
#define X808_GAIN 52000.0f
void x808_run(void *st, int lane, int32_t *y, int n)
{
    x808_t *x = (x808_t *)st;
    float buf[X808_BLOCK], acc[X808_BLOCK], snd[X808_BLOCK];   /* snd: the 808's sends, not used here */
    int i, k;
    if (n > X808_BLOCK)
        n = X808_BLOCK;
    for (i = 0; i < n; i++)
        y[i] = 0;
    if (x->magic != X808_MAGIC || lane < 0 || lane >= X8_NLANE)
        return;
    for (i = 0; i < n; i++)
        acc[i] = 0.0f;
    for (k = 0; k < 3; k++) {
        int l = X8_LANES[lane][k], mm = n, got;
        d8_lane_t *ln;
        if (l >= L_NUM || !lane_on(&x->d, l))
            continue;
        if (l >= L_CB && !x->bank_on) {           /* struck after this block's x808_block: the bank from here */
            bank_block(&x->d.bank, x->bus, x->pair, X808_BLOCK);
            x->bank_on = 1;
        }
        ln = &x->d.lane[l];
        if (ln->cstep < 0.0f) {                   /* (d8_render: a choked voice stops where its fade ends) */
            float c = ln->cg;
            for (i = 0; i < n; i++) {
                c += ln->cstep;
                if (c <= 0.0f) {
                    mm = i + 1;
                    break;
                }
            }
        }
        got = voice_run(&x->d, l, buf, mm, x->bus, x->pair);
        d8_dry_r = 0;
        lane_mix(&x->d, ln, buf, got, acc, snd, snd);
#if D8_TAIL_DB
        lane_tail(&x->d, l, buf, got);
#endif
    }
    for (i = 0; i < n; i++) {
        float v = acc[i] * (X808_GAIN * X8_TRIM[lane]);
        y[i] = v > 30000.0f ? 30000 : v < -30000.0f ? -30000 : (int32_t)v;
    }
}

/* host unity build (the tests include this file from eng_drum.c): drum808.c's short macro names end here */
#undef BANK_DUTY
#undef BANK_NONE
#undef BANK_SUB
#undef BD_Q
#undef CHUNK
#undef CL_F
#undef CL_Q
#undef CP_F
#undef CP_Q
#undef CTRL_RATE
#undef DIODE_VON
#undef DRV
#undef DST
#undef FM_LN2
#undef FM_LOG2E
#undef FM_PI
#undef FM_TWO_PI
#undef LVL
#undef MA_ATK_DEF
#undef NON
#undef PI_SR
#undef PP
#undef PUL_DUTY
#undef P_DIST
#undef P_DLY
#undef P_DRIVE
#undef P_REV
#undef QUIET
#undef SC808_FULL_VELOCITY_GAIN
#undef SD_DECAY_DEF
#undef SD_F1
#undef SD_F2
#undef SD_G1
#undef SD_G2
#undef SD_Q1
#undef SD_Q2
#undef SH_A
#undef SH_DC
#undef SND
#undef SR
#undef TOM_LN100_Q_PI
#undef TOM_Q
#undef TRK
#undef TU12
#undef TU2
#undef TUM
#undef TWO_PI_SR
#include "x808/ns_end.h"
