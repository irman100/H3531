#ifndef H3531_RACER_AUDIO_H
#define H3531_RACER_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

int racer_audio_start(const char *asset_dir);
void racer_audio_stop(void);
int racer_audio_is_active(void);

void racer_audio_update_vehicle(
    float speed,float max_speed,float throttle,
    unsigned gear,unsigned gears,int brake,int handbrake,float slip,
    unsigned wheel_state_bits,unsigned surface_type);
enum {
    RACER_AUDIO_IMPACT_WALL = 0,
    RACER_AUDIO_IMPACT_LAND = 1
};
void racer_audio_collision(float strength,int kind);
void racer_audio_radio_cycle(void);
void racer_audio_radio_step(int delta);
void racer_audio_set_radio_volume(int percent);
int racer_audio_radio_volume(void);
int racer_audio_radio_index(void);
int racer_audio_radio_enabled(void);
int racer_audio_radio_available(void);

#ifdef __cplusplus
}
#endif
#endif
