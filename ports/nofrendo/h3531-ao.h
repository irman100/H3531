#ifndef H3531_NOFRENDO_AO_H
#define H3531_NOFRENDO_AO_H

#include <stdint.h>

int h3531_ao_start(void);
int h3531_ao_send_160(const int16_t *pcm);
void h3531_ao_stop(void);

#endif
