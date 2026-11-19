#ifndef GAME_WS_H
#define GAME_WS_H

/* Widescreen enhancement ([Graphics] Widescreen = 1). See game_ws.c. */

/* SnesDesktopHostGame.prepare_frame: the frame width for this run. */
void SgngWsPrepareFrame(int drawable_w, int drawable_h, int *frame_w, int *frame_h);

/* Rasterise one field with this frame's widescreen policy applied. Replaces
 * the plain beam-driver draw; identical to it when widescreen is off. */
void SgngWsDrawPpuFrame(void);

/* Console reset: forget everything known about the tilemaps. */
void SgngWsReset(void);

#endif /* GAME_WS_H */
