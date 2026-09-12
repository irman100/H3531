#ifndef H3531_AUDIO_H
#define H3531_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int h3531_audio_start(void);
void h3531_audio_submit_u8_mono(const uint8_t *samples, size_t count);
void h3531_audio_pace_frame(int turbo);
void h3531_audio_stop(void);
int h3531_audio_is_active(void);

#ifdef __cplusplus
}
#endif

#endif
