/* Overlays for build/port/play: port/overlay.c. */
#ifndef OVERLAY_H
#define OVERLAY_H
#include <stdint.h>

enum { OV_W = 1920, OV_H = 1080 };

/* Load an overlay file (agentrun's SBM2_OVERLAY); returns its events or -1. */
int ov_load(const char *path);
/* A futures window: one oracle run (an S event) replayed by the host from
 * the decision's frame. pix: its picture now (256x224 XRGB); age: frames
 * played; ended: its pads ran out (the picture holds); pad: its pad now. */
typedef struct { const uint32_t *pix; int ev; int age; int ended; uint16_t pad; } OvWin;
enum { OV_MAXWIN = 4 };
/* After ov_frame's events for vf: a new set of futures to show, if the
 * windows should change now (a decision's runs at vf, and the windows all
 * ended or the goal changed): fills evs, returns how many (0: keep). */
int ov_pick(uint32_t vf, int all_ended, int *evs);
/* An S event's pads (one per frame from the decision) and death offset. */
const uint16_t *ov_pads(int ev, int *n);
int ov_dead(int ev);

/* Draw video frame vf: the game picture (256x224 XRGB), WRAM and P1's pad,
 * onto canvas (OV_W x OV_H XRGB). Call once per frame, in order. */
void ov_frame(const uint8_t *wram, uint32_t vf, const uint32_t *game, uint16_t pad, const OvWin *win, int nwin,
              uint32_t *canvas);

#endif
