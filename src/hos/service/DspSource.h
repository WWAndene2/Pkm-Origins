/* The DSP's sound sources' statuses each audio frame: see DspSource.c. */
#ifndef HOS_DSP_SOURCE_H
#define HOS_DSP_SOURCE_H

/* one audio frame: 1 when made (the statuses written; the game's audio interrupt is then due), 0 when put off because the
   game has not written a new frame since the last one (its frame counter unchanged) */
int hos_dsp_sources_tick(void);

#endif
