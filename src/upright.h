/* upright.h: a small tilted sprite (a sword lying diagonally) stood upright at 32 x 32, never blending a colour.
   The turn is cleanEdge (torcado's pixel-art shader, MIT), then optional outline steps and a slimmer grip. */
#ifndef UPRIGHT_H
#define UPRIGHT_H

enum { UP_SLIM_OFF, UP_SLIM_SWORD, UP_SLIM_SHAFT };
typedef struct {
    int auto_turn;      /* 1: measure the tilt (long axis, snapped to 45 / 26.6 / 63.4 within 8 degrees) */
    double turn;        /* else: degrees clockwise (45 stands up a sword whose point is at the upper left) */
    int centred;        /* 0 classic (blade centre between two pixel columns), 1 centred (one-pixel point) */
    int slim;           /* UP_SLIM_*: the grip (below the crossguard) or the shaft one pixel slimmer each side */
    int outline;        /* -1: the outline steps only when the original is fenced in by black; 0 never; 1 always */
} UprightOpt;

void upright_defaults(UprightOpt *o);
/* the long axis: tilt in degrees (+ leans right, - leans left), elongation (long / short), and whether it counts as
   tilted (stretched at least 1.6 times, 20 to 70 degrees off level, at least 6 pixels); returns 0 for an empty sprite */
int upright_measure(const unsigned char *rgba, int w, int h, double *tilt, double *elong, int *tilted);
/* the sprite stood upright, centred in 32 x 32 RGBA (out32); the turn and the scale used come back (either may be NULL) */
int upright_make(const unsigned char *rgba, int w, int h, const UprightOpt *o, unsigned char *out32, double *turn_used,
                 double *scale_used);
/* the same turn drawn large (long side about `size` pixels), cropped, no outline steps: the clean picture for the
   image models. Returns malloc'd RGBA (the caller frees it) */
unsigned char *upright_big(const unsigned char *rgba, int w, int h, double turn, int size, int *ow, int *oh);

#endif
