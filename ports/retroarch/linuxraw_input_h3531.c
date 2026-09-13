/* RetroArch H3531 evdev input PoC.
 *
 * Replaces RetroArch's Linux "linuxraw" stdin/VT-scancode implementation
 * during the H3531 proof-of-concept build.  The public driver name is
 * "h3531evdev".  It reads a USB keyboard directly from /dev/input/event*.
 *
 * Stage-1 scope is keyboard/menu navigation.  USB joypad autoconfiguration
 * is intentionally deferred to the next stage after the menu is proven on
 * real hardware.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include "../../verbosity.h"
#include "../input_keymaps.h"
#include "../input_driver.h"

#define H3531_EVENT_MAX 64
#define H3531_BITS_PER_LONG (8U * (unsigned)sizeof(unsigned long))
#define H3531_NBITS(x) (((x) + H3531_BITS_PER_LONG - 1U) / H3531_BITS_PER_LONG)
#define H3531_TEST_BIT(bit, arr) \
   (((arr)[(unsigned)(bit) / H3531_BITS_PER_LONG] >> \
      ((unsigned)(bit) % H3531_BITS_PER_LONG)) & 1UL)

typedef struct linuxraw_input
{
   bool state[KEY_MAX + 1];
   int fd;
   uint64_t next_scan_ms;
   char path[64];
} linuxraw_input_t;

static uint64_t h3531_now_ms(void)
{
   struct timeval tv;
   gettimeofday(&tv, NULL);
   return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static int h3531_contains_ci(const char *s, const char *needle)
{
   size_t i, j;
   if (!s || !needle || !needle[0])
      return 0;

   for (i = 0; s[i]; ++i)
   {
      for (j = 0; needle[j] && s[i + j]; ++j)
      {
         char a = s[i + j];
         char b = needle[j];
         if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
         if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
         if (a != b)
            break;
      }
      if (!needle[j])
         return 1;
   }
   return 0;
}

static bool h3531_keyboard_capable(int fd, const char *name)
{
   unsigned long evbits[H3531_NBITS(EV_MAX + 1)];
   unsigned long keybits[H3531_NBITS(KEY_MAX + 1)];
   unsigned score = 0;

   memset(evbits, 0, sizeof(evbits));
   memset(keybits, 0, sizeof(keybits));

   if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0)
      return false;
   if (!H3531_TEST_BIT(EV_KEY, evbits))
      return false;
   if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0)
      return false;

   if (H3531_TEST_BIT(KEY_A, keybits)) score++;
   if (H3531_TEST_BIT(KEY_Z, keybits)) score++;
   if (H3531_TEST_BIT(KEY_ENTER, keybits)) score++;
   if (H3531_TEST_BIT(KEY_ESC, keybits)) score++;
   if (H3531_TEST_BIT(KEY_UP, keybits)) score++;
   if (H3531_TEST_BIT(KEY_DOWN, keybits)) score++;
   if (H3531_TEST_BIT(KEY_LEFT, keybits)) score++;
   if (H3531_TEST_BIT(KEY_RIGHT, keybits)) score++;

   if (h3531_contains_ci(name, "keyboard") || h3531_contains_ci(name, "kbd"))
      return score >= 3;

   return score >= 7;
}

static int h3531_open_keyboard(char *chosen, size_t chosen_len)
{
   const char *forced = getenv("RETROARCH_INPUT");
   int i;

   if (forced && forced[0])
   {
      int fd = open(forced, O_RDONLY | O_NONBLOCK);
      if (fd >= 0)
      {
         snprintf(chosen, chosen_len, "%s", forced);
         RARCH_LOG("[H3531] evdev keyboard forced: %s\n", forced);
         return fd;
      }
      RARCH_WARN("[H3531] cannot open RETROARCH_INPUT=%s: %s\n",
            forced, strerror(errno));
   }

   for (i = 0; i < H3531_EVENT_MAX; ++i)
   {
      char path[64];
      char name[128];
      int fd;

      snprintf(path, sizeof(path), "/dev/input/event%d", i);
      fd = open(path, O_RDONLY | O_NONBLOCK);
      if (fd < 0)
         continue;

      memset(name, 0, sizeof(name));
      if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
         snprintf(name, sizeof(name), "event%d", i);

      if (h3531_keyboard_capable(fd, name))
      {
         snprintf(chosen, chosen_len, "%s", path);
         RARCH_LOG("[H3531] evdev keyboard: %s (%s)\n", path, name);
         return fd;
      }

      close(fd);
   }

   return -1;
}

static void h3531_try_rescan(linuxraw_input_t *in)
{
   uint64_t now;
   if (!in || in->fd >= 0)
      return;

   now = h3531_now_ms();
   if (now < in->next_scan_ms)
      return;

   in->next_scan_ms = now + 1000ULL;
   in->fd = h3531_open_keyboard(in->path, sizeof(in->path));
   if (in->fd < 0)
      RARCH_WARN("[H3531] no evdev keyboard found; will rescan\n");
}

static void *linuxraw_input_init(const char *joypad_driver)
{
   linuxraw_input_t *in = (linuxraw_input_t*)calloc(1, sizeof(*in));
   (void)joypad_driver;

   if (!in)
      return NULL;

   in->fd = -1;
   input_keymaps_init_keyboard_lut(rarch_key_map_linux);
   h3531_try_rescan(in);

   RARCH_LOG("[H3531] RetroArch evdev input initialized\n");
   return in;
}

static bool h3531_key_down(const linuxraw_input_t *in, enum retro_key key)
{
   unsigned sym;
   if (!in || key <= RETROK_UNKNOWN || key >= RETROK_LAST)
      return false;

   sym = rarch_keysym_lut[key];
   if (sym > KEY_MAX)
      return false;
   return in->state[sym];
}

static int16_t linuxraw_input_state(
      void *data,
      const input_device_driver_t *joypad,
      const input_device_driver_t *sec_joypad,
      rarch_joypad_info_t *joypad_info,
      const retro_keybind_set *binds,
      bool keyboard_mapping_blocked,
      unsigned port,
      unsigned device,
      unsigned idx,
      unsigned id)
{
   linuxraw_input_t *in = (linuxraw_input_t*)data;
   (void)joypad;
   (void)sec_joypad;
   (void)joypad_info;

   if (!in)
      return 0;

   switch (device)
   {
      case RETRO_DEVICE_JOYPAD:
         if (!binds || keyboard_mapping_blocked)
            return 0;

         if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
         {
            unsigned i;
            int16_t ret = 0;
            for (i = 0; i < RARCH_FIRST_CUSTOM_BIND; ++i)
            {
               enum retro_key key;
               if (!RETRO_KEYBIND_VALID(&binds[port][i]))
                  continue;
               key = (enum retro_key)RETRO_KEYBIND_KEY(&binds[port][i]);
               if (h3531_key_down(in, key))
                  ret |= (int16_t)(1U << i);
            }
            return ret;
         }

         if (id < RARCH_BIND_LIST_END && RETRO_KEYBIND_VALID(&binds[port][id]))
         {
            enum retro_key key =
               (enum retro_key)RETRO_KEYBIND_KEY(&binds[port][id]);
            return h3531_key_down(in, key) ? 1 : 0;
         }
         break;

      case RETRO_DEVICE_ANALOG:
         if (binds)
         {
            unsigned id_minus = 0;
            unsigned id_plus  = 0;
            int16_t ret = 0;
            input_conv_analog_id_to_bind_id(idx, id, id_minus, id_plus);

            if (RETRO_KEYBIND_VALID(&binds[port][id_plus]) &&
                h3531_key_down(in, (enum retro_key)
                  RETRO_KEYBIND_KEY(&binds[port][id_plus])))
               ret = 0x7fff;

            if (RETRO_KEYBIND_VALID(&binds[port][id_minus]) &&
                h3531_key_down(in, (enum retro_key)
                  RETRO_KEYBIND_KEY(&binds[port][id_minus])))
               ret = (int16_t)(ret - 0x7fff);

            return ret;
         }
         break;

      case RETRO_DEVICE_KEYBOARD:
         if (id && id < RETROK_LAST)
            return h3531_key_down(in, (enum retro_key)id) ? 1 : 0;
         break;
   }

   return 0;
}

static void linuxraw_input_poll(void *data)
{
   linuxraw_input_t *in = (linuxraw_input_t*)data;
   struct input_event ev;

   if (!in)
      return;

   h3531_try_rescan(in);
   if (in->fd < 0)
      return;

   for (;;)
   {
      ssize_t n = read(in->fd, &ev, sizeof(ev));
      if (n == (ssize_t)sizeof(ev))
      {
         if (ev.type == EV_KEY && ev.code <= KEY_MAX &&
             (ev.value == 0 || ev.value == 1 || ev.value == 2))
         {
            bool pressed = ev.value != 0;
            enum retro_key key;
            uint16_t mod = 0;

            in->state[ev.code] = pressed;

            if (in->state[KEY_LEFTCTRL] || in->state[KEY_RIGHTCTRL])
               mod |= RETROKMOD_CTRL;
            if (in->state[KEY_LEFTALT] || in->state[KEY_RIGHTALT])
               mod |= RETROKMOD_ALT;
            if (in->state[KEY_LEFTSHIFT] || in->state[KEY_RIGHTSHIFT])
               mod |= RETROKMOD_SHIFT;
            if (in->state[KEY_LEFTMETA] || in->state[KEY_RIGHTMETA])
               mod |= RETROKMOD_META;

            key = input_keymaps_translate_keysym_to_rk(ev.code);
            if (key > RETROK_UNKNOWN && key < RETROK_LAST)
               input_keyboard_event(pressed, key, key, mod,
                     RETRO_DEVICE_KEYBOARD);
         }
         continue;
      }

      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
         break;

      RARCH_WARN("[H3531] evdev keyboard disconnected/read error on %s\n",
            in->path[0] ? in->path : "event device");
      close(in->fd);
      in->fd = -1;
      memset(in->state, 0, sizeof(in->state));
      in->next_scan_ms = h3531_now_ms() + 500ULL;
      break;
   }
}

static void linuxraw_input_free(void *data)
{
   linuxraw_input_t *in = (linuxraw_input_t*)data;
   if (!in)
      return;
   if (in->fd >= 0)
      close(in->fd);
   free(in);
}

static bool linuxraw_input_set_sensor_state(void *data, unsigned port,
      enum retro_sensor_action action, unsigned rate)
{
   (void)data; (void)port; (void)action; (void)rate;
   return false;
}

static float linuxraw_input_get_sensor_input(void *data, unsigned port,
      unsigned id)
{
   (void)data; (void)port; (void)id;
   return 0.0f;
}

static uint64_t linuxraw_get_capabilities(void *data)
{
   (void)data;
   return (1ULL << RETRO_DEVICE_JOYPAD)
        | (1ULL << RETRO_DEVICE_ANALOG)
        | (1ULL << RETRO_DEVICE_KEYBOARD);
}

/* Kept as input_linuxraw so upstream's normal Linux registration remains
 * untouched in the first PoC. */
input_driver_t input_linuxraw = {
   linuxraw_input_init,
   linuxraw_input_poll,
   linuxraw_input_state,
   linuxraw_input_free,
   linuxraw_input_set_sensor_state,
   linuxraw_input_get_sensor_input,
   linuxraw_get_capabilities,
   "h3531evdev",
   NULL,
   NULL,
   NULL
};
