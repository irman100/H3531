/* RetroArch H3531 evdev input PoC.
 *
 * Replaces RetroArch's Linux "linuxraw" stdin/VT-scancode implementation
 * during the H3531 proof-of-concept build.  The public driver name is
 * "h3531evdev".  It reads a USB keyboard directly from /dev/input/event*.
 *
 * Stage6.8 input scope: keyboard plus generic USB evdev gamepads.
 * Gamepads are handled inside the primary input driver so RGUI and libretro
 * cores share the same zero-config hotplug path without udev/SDL.
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
#define H3531_MAX_PADS 4
#define H3531_BITS_PER_LONG (8U * (unsigned)sizeof(unsigned long))
#define H3531_NBITS(x) (((x) + H3531_BITS_PER_LONG - 1U) / H3531_BITS_PER_LONG)
#define H3531_TEST_BIT(bit, arr) \
   (((arr)[(unsigned)(bit) / H3531_BITS_PER_LONG] >> \
      ((unsigned)(bit) % H3531_BITS_PER_LONG)) & 1UL)

typedef struct h3531_pad
{
   int fd;
   char path[64];
   char name[128];
   bool key[KEY_MAX + 1];
   int abs[ABS_MAX + 1];
   struct input_absinfo absinfo[ABS_MAX + 1];
   bool abs_valid[ABS_MAX + 1];
} h3531_pad_t;

typedef struct linuxraw_input
{
   bool state[KEY_MAX + 1];
   int fd;
   uint64_t next_scan_ms;
   char path[64];

   h3531_pad_t pads[H3531_MAX_PADS];
   uint64_t next_pad_scan_ms;
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

static bool h3531_gamepad_capable(int fd, const char *name)
{
   unsigned long evbits[H3531_NBITS(EV_MAX + 1)];
   unsigned long keybits[H3531_NBITS(KEY_MAX + 1)];
   unsigned long absbits[H3531_NBITS(ABS_MAX + 1)];
   unsigned buttons = 0;
   bool axes = false;
   bool dpad = false;
   bool name_hint;

   memset(evbits, 0, sizeof(evbits));
   memset(keybits, 0, sizeof(keybits));
   memset(absbits, 0, sizeof(absbits));

   if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0)
      return false;

   if (H3531_TEST_BIT(EV_KEY, evbits))
   {
      static const unsigned game_keys[] = {
         BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST,
         BTN_TL, BTN_TR, BTN_SELECT, BTN_START,
         BTN_TRIGGER, BTN_THUMB, BTN_THUMB2, BTN_TOP, BTN_TOP2
      };
      unsigned i;

      if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0)
         return false;

      for (i = 0; i < sizeof(game_keys) / sizeof(game_keys[0]); ++i)
         if (H3531_TEST_BIT(game_keys[i], keybits))
            buttons++;

      dpad = H3531_TEST_BIT(BTN_DPAD_LEFT, keybits) ||
             H3531_TEST_BIT(BTN_DPAD_RIGHT, keybits) ||
             H3531_TEST_BIT(BTN_DPAD_UP, keybits) ||
             H3531_TEST_BIT(BTN_DPAD_DOWN, keybits);
   }

   if (H3531_TEST_BIT(EV_ABS, evbits))
   {
      if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) < 0)
         return false;

      axes = (H3531_TEST_BIT(ABS_X, absbits) &&
              H3531_TEST_BIT(ABS_Y, absbits)) ||
             (H3531_TEST_BIT(ABS_HAT0X, absbits) &&
              H3531_TEST_BIT(ABS_HAT0Y, absbits));
   }

   name_hint = h3531_contains_ci(name, "gamepad") ||
               h3531_contains_ci(name, "joystick") ||
               h3531_contains_ci(name, "controller") ||
               h3531_contains_ci(name, "game stick");

   return (buttons >= 2 && (axes || dpad)) ||
          (name_hint && buttons >= 1 && (axes || dpad));
}

static bool h3531_path_used(const linuxraw_input_t *in, const char *path)
{
   unsigned i;

   if (!in || !path)
      return false;

   if (in->fd >= 0 && strcmp(in->path, path) == 0)
      return true;

   for (i = 0; i < H3531_MAX_PADS; ++i)
      if (in->pads[i].fd >= 0 && strcmp(in->pads[i].path, path) == 0)
         return true;

   return false;
}

static void h3531_pad_reset(h3531_pad_t *pad)
{
   if (!pad)
      return;
   memset(pad, 0, sizeof(*pad));
   pad->fd = -1;
}

static void h3531_pad_prepare(h3531_pad_t *pad)
{
   unsigned code;

   if (!pad || pad->fd < 0)
      return;

   for (code = 0; code <= ABS_MAX; ++code)
   {
      struct input_absinfo ai;
      if (ioctl(pad->fd, EVIOCGABS(code), &ai) == 0)
      {
         pad->absinfo[code] = ai;
         pad->abs[code] = ai.value;
         pad->abs_valid[code] = true;
      }
   }
}

static void h3531_scan_gamepads(linuxraw_input_t *in)
{
   int event_no;

   if (!in)
      return;

   for (event_no = 0; event_no < H3531_EVENT_MAX; ++event_no)
   {
      char path[64];
      char name[128];
      int fd;
      int slot = -1;
      unsigned p;

      for (p = 0; p < H3531_MAX_PADS; ++p)
         if (in->pads[p].fd < 0)
         {
            slot = (int)p;
            break;
         }

      if (slot < 0)
         return;

      snprintf(path, sizeof(path), "/dev/input/event%d", event_no);
      if (h3531_path_used(in, path))
         continue;

      fd = open(path, O_RDONLY | O_NONBLOCK);
      if (fd < 0)
         continue;

      memset(name, 0, sizeof(name));
      if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
         snprintf(name, sizeof(name), "event%d", event_no);

      if (!h3531_gamepad_capable(fd, name))
      {
         close(fd);
         continue;
      }

      h3531_pad_reset(&in->pads[slot]);
      in->pads[slot].fd = fd;
      snprintf(in->pads[slot].path, sizeof(in->pads[slot].path), "%s", path);
      snprintf(in->pads[slot].name, sizeof(in->pads[slot].name), "%s", name);
      h3531_pad_prepare(&in->pads[slot]);

      if (ioctl(fd, EVIOCGRAB, (void*)1) < 0)
         RARCH_WARN("[H3531] gamepad%d EVIOCGRAB failed on %s: %s\n",
               slot + 1, path, strerror(errno));

      RARCH_LOG("[H3531] gamepad%d connected: %s (%s)\n",
            slot + 1, path, name);
   }
}

static void h3531_try_pad_rescan(linuxraw_input_t *in)
{
   uint64_t now;

   if (!in)
      return;

   now = h3531_now_ms();
   if (now < in->next_pad_scan_ms)
      return;

   in->next_pad_scan_ms = now + 1000ULL;
   h3531_scan_gamepads(in);
}

static bool h3531_pad_key(const h3531_pad_t *pad, unsigned code)
{
   return pad && code <= KEY_MAX && pad->key[code];
}

static int h3531_axis_zone(const h3531_pad_t *pad, unsigned code)
{
   int minimum, maximum, center, threshold, value;

   if (!pad || code > ABS_MAX || !pad->abs_valid[code])
      return 0;

   minimum = pad->absinfo[code].minimum;
   maximum = pad->absinfo[code].maximum;
   value   = pad->abs[code];

   if (maximum <= minimum)
      return 0;

   center = minimum + (maximum - minimum) / 2;
   threshold = (maximum - minimum) / 4;
   if (threshold < 1)
      threshold = 1;

   if (value < center - threshold)
      return -1;
   if (value > center + threshold)
      return 1;
   return 0;
}

static bool h3531_pad_button(const h3531_pad_t *pad, unsigned id)
{
   int hx, hy, lx, ly;

   if (!pad || pad->fd < 0)
      return false;

   hx = h3531_axis_zone(pad, ABS_HAT0X);
   hy = h3531_axis_zone(pad, ABS_HAT0Y);
   lx = h3531_axis_zone(pad, ABS_X);
   ly = h3531_axis_zone(pad, ABS_Y);

   switch (id)
   {
      case RETRO_DEVICE_ID_JOYPAD_B:
         return h3531_pad_key(pad, BTN_SOUTH) ||
                h3531_pad_key(pad, BTN_TRIGGER);
      case RETRO_DEVICE_ID_JOYPAD_A:
         return h3531_pad_key(pad, BTN_EAST) ||
                h3531_pad_key(pad, BTN_THUMB);
      case RETRO_DEVICE_ID_JOYPAD_Y:
         return h3531_pad_key(pad, BTN_WEST) ||
                h3531_pad_key(pad, BTN_TOP);
      case RETRO_DEVICE_ID_JOYPAD_X:
         return h3531_pad_key(pad, BTN_NORTH) ||
                h3531_pad_key(pad, BTN_TOP2);
      case RETRO_DEVICE_ID_JOYPAD_SELECT:
         return h3531_pad_key(pad, BTN_SELECT) ||
                h3531_pad_key(pad, BTN_BASE);
      case RETRO_DEVICE_ID_JOYPAD_START:
         return h3531_pad_key(pad, BTN_START) ||
                h3531_pad_key(pad, BTN_BASE2);
      case RETRO_DEVICE_ID_JOYPAD_L:
         return h3531_pad_key(pad, BTN_TL) ||
                h3531_pad_key(pad, BTN_BASE3);
      case RETRO_DEVICE_ID_JOYPAD_R:
         return h3531_pad_key(pad, BTN_TR) ||
                h3531_pad_key(pad, BTN_BASE4);
      case RETRO_DEVICE_ID_JOYPAD_L2:
         return h3531_pad_key(pad, BTN_TL2) ||
                h3531_pad_key(pad, BTN_BASE5);
      case RETRO_DEVICE_ID_JOYPAD_R2:
         return h3531_pad_key(pad, BTN_TR2) ||
                h3531_pad_key(pad, BTN_BASE6);
      case RETRO_DEVICE_ID_JOYPAD_L3:
         return h3531_pad_key(pad, BTN_THUMBL);
      case RETRO_DEVICE_ID_JOYPAD_R3:
         return h3531_pad_key(pad, BTN_THUMBR);
      case RETRO_DEVICE_ID_JOYPAD_LEFT:
         return h3531_pad_key(pad, BTN_DPAD_LEFT) || hx < 0 ||
                (!pad->abs_valid[ABS_HAT0X] && lx < 0);
      case RETRO_DEVICE_ID_JOYPAD_RIGHT:
         return h3531_pad_key(pad, BTN_DPAD_RIGHT) || hx > 0 ||
                (!pad->abs_valid[ABS_HAT0X] && lx > 0);
      case RETRO_DEVICE_ID_JOYPAD_UP:
         return h3531_pad_key(pad, BTN_DPAD_UP) || hy < 0 ||
                (!pad->abs_valid[ABS_HAT0Y] && ly < 0);
      case RETRO_DEVICE_ID_JOYPAD_DOWN:
         return h3531_pad_key(pad, BTN_DPAD_DOWN) || hy > 0 ||
                (!pad->abs_valid[ABS_HAT0Y] && ly > 0);
      default:
         return false;
   }
}

static int16_t h3531_pad_axis(const h3531_pad_t *pad, unsigned code)
{
   int minimum, maximum, center, value;
   int64_t scaled;

   if (!pad || pad->fd < 0 || code > ABS_MAX || !pad->abs_valid[code])
      return 0;

   minimum = pad->absinfo[code].minimum;
   maximum = pad->absinfo[code].maximum;
   value   = pad->abs[code];

   if (maximum <= minimum)
      return 0;

   center = minimum + (maximum - minimum) / 2;

   if (value >= center)
   {
      int span = maximum - center;
      if (span <= 0)
         return 0;
      scaled = (int64_t)(value - center) * 32767 / span;
   }
   else
   {
      int span = center - minimum;
      if (span <= 0)
         return 0;
      scaled = -((int64_t)(center - value) * 32767 / span);
   }

   if (scaled > -4096 && scaled < 4096)
      return 0;
   if (scaled > 32767)
      scaled = 32767;
   if (scaled < -32767)
      scaled = -32767;
   return (int16_t)scaled;
}

static int16_t h3531_pad_analog(const h3531_pad_t *pad, unsigned idx, unsigned id)
{
   unsigned code;

   if (!pad)
      return 0;

   if (idx == RETRO_DEVICE_INDEX_ANALOG_LEFT)
      code = (id == RETRO_DEVICE_ID_ANALOG_Y) ? ABS_Y : ABS_X;
   else if (idx == RETRO_DEVICE_INDEX_ANALOG_RIGHT)
      code = (id == RETRO_DEVICE_ID_ANALOG_Y) ? ABS_RY : ABS_RX;
   else
      return 0;

   return h3531_pad_axis(pad, code);
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
         if (ioctl(fd, EVIOCGRAB, (void*)1) < 0)
         {
            RARCH_WARN("[H3531] EVIOCGRAB failed on %s: %s\n",
                  forced, strerror(errno));
         }
         else
            RARCH_LOG("[H3531] Stage6.6D exclusive evdev grab: %s\n", forced);
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
         if (ioctl(fd, EVIOCGRAB, (void*)1) < 0)
         {
            RARCH_WARN("[H3531] EVIOCGRAB failed on %s: %s\n",
                  path, strerror(errno));
         }
         else
            RARCH_LOG("[H3531] Stage6.6D exclusive evdev grab: %s\n", path);
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

   {
      unsigned i;
      in->fd = -1;
      for (i = 0; i < H3531_MAX_PADS; ++i)
         h3531_pad_reset(&in->pads[i]);
   }
   in->next_pad_scan_ms = 0;
   input_keymaps_init_keyboard_lut(rarch_key_map_linux);
   h3531_try_rescan(in);
   h3531_try_pad_rescan(in);

   RARCH_LOG("[H3531] RetroArch evdev keyboard + USB gamepad input initialized\n");
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
   const h3531_pad_t *pad = NULL;
   (void)joypad;
   (void)sec_joypad;
   (void)joypad_info;

   if (!in)
      return 0;

   if (port < H3531_MAX_PADS && in->pads[port].fd >= 0)
      pad = &in->pads[port];

   switch (device)
   {
      case RETRO_DEVICE_JOYPAD:
         if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
         {
            unsigned i;
            int16_t ret = 0;

            for (i = 0; i < 16; ++i)
               if (h3531_pad_button(pad, i))
                  ret |= (int16_t)(1U << i);

            if (port == 0 && binds && !keyboard_mapping_blocked)
            {
               for (i = 0; i < RARCH_FIRST_CUSTOM_BIND && i < 16; ++i)
               {
                  enum retro_key key;
                  if (!RETRO_KEYBIND_VALID(&binds[port][i]))
                     continue;
                  key = (enum retro_key)RETRO_KEYBIND_KEY(&binds[port][i]);
                  if (h3531_key_down(in, key))
                     ret |= (int16_t)(1U << i);
               }
            }
            return ret;
         }

         if (h3531_pad_button(pad, id))
            return 1;

         if (port == 0 && binds && !keyboard_mapping_blocked &&
             id < RARCH_BIND_LIST_END && RETRO_KEYBIND_VALID(&binds[port][id]))
         {
            enum retro_key key =
               (enum retro_key)RETRO_KEYBIND_KEY(&binds[port][id]);
            return h3531_key_down(in, key) ? 1 : 0;
         }
         break;

      case RETRO_DEVICE_ANALOG:
      {
         int16_t physical = h3531_pad_analog(pad, idx, id);
         if (physical)
            return physical;

         if (port == 0 && binds)
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
      }

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
   h3531_try_pad_rescan(in);

   if (in->fd >= 0)
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

      if (n < 0)
      {
         int read_errno = errno;

         /* Signals are normal in RetroArch and must never be interpreted as
          * an evdev disconnect. The old H3531 PoC closed the keyboard on
          * EINTR, causing unnecessary event0/event1 rescans. */
         if (read_errno == EAGAIN || read_errno == EWOULDBLOCK ||
             read_errno == EINTR)
            break;

         /* Only kernel/device-loss errors trigger a real reconnect. */
         if (read_errno != ENODEV && read_errno != ENXIO &&
             read_errno != EIO && read_errno != EBADF)
         {
            RARCH_WARN("[H3531] transient evdev read error on %s: "
                  "errno=%d (%s); keeping fd\n",
                  in->path[0] ? in->path : "event device",
                  read_errno, strerror(read_errno));
            break;
         }

         RARCH_WARN("[H3531] evdev keyboard disconnected on %s: "
               "errno=%d (%s)\n",
               in->path[0] ? in->path : "event device",
               read_errno, strerror(read_errno));
      }
      else if (n == 0)
      {
         RARCH_WARN("[H3531] evdev keyboard EOF on %s; reconnecting\n",
               in->path[0] ? in->path : "event device");
      }
      else
      {
         RARCH_WARN("[H3531] short evdev read on %s: %ld bytes; "
               "keeping fd\n",
               in->path[0] ? in->path : "event device", (long)n);
         break;
      }

      ioctl(in->fd, EVIOCGRAB, (void*)0);
      close(in->fd);
      in->fd = -1;
      memset(in->state, 0, sizeof(in->state));
      in->next_scan_ms = h3531_now_ms() + 500ULL;
      break;
   }

   {
      unsigned p;
      for (p = 0; p < H3531_MAX_PADS; ++p)
      {
         h3531_pad_t *pad = &in->pads[p];

         if (pad->fd < 0)
            continue;

         for (;;)
         {
            struct input_event pev;
            ssize_t n = read(pad->fd, &pev, sizeof(pev));

            if (n == (ssize_t)sizeof(pev))
            {
               if (pev.type == EV_KEY && pev.code <= KEY_MAX &&
                   (pev.value == 0 || pev.value == 1 || pev.value == 2))
                  pad->key[pev.code] = pev.value != 0;
               else if (pev.type == EV_ABS && pev.code <= ABS_MAX)
                  pad->abs[pev.code] = pev.value;
               continue;
            }

            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                          errno == EINTR))
               break;

            if (n <= 0)
            {
               RARCH_WARN("[H3531] gamepad%u disconnected on %s; rescanning\n",
                     p + 1, pad->path[0] ? pad->path : "event device");
               ioctl(pad->fd, EVIOCGRAB, (void*)0);
               close(pad->fd);
               h3531_pad_reset(pad);
               in->next_pad_scan_ms = h3531_now_ms() + 500ULL;
            }
            break;
         }
      }
   }
}

static void linuxraw_input_free(void *data)
{
   linuxraw_input_t *in = (linuxraw_input_t*)data;
   unsigned i;

   if (!in)
      return;

   if (in->fd >= 0)
   {
      ioctl(in->fd, EVIOCGRAB, (void*)0);
      close(in->fd);
   }

   for (i = 0; i < H3531_MAX_PADS; ++i)
   {
      if (in->pads[i].fd >= 0)
      {
         ioctl(in->pads[i].fd, EVIOCGRAB, (void*)0);
         close(in->pads[i].fd);
      }
   }

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
