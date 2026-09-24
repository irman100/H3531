/* H3531 RetroArch linuxraw joypad transport.
 *
 * Keeps the standard RetroArch joypad driver identity "linuxraw" and standard
 * autoconfig files, but supports kernels where CONFIG_INPUT_JOYDEV is absent
 * and therefore /dev/input/js* does not exist.
 *
 * Transport:
 *   1. Prefer stock /dev/input/jsN when present.
 *   2. Otherwise discover gamepad-capable /dev/input/eventN nodes.
 *   3. event hotplug is driven by inotify; active descriptors by epoll.
 *   4. No periodic event0..63 scan runs from the gameplay poll loop.
 */

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <stdbool.h>

#include <sys/types.h>
#include <sys/inotify.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>

#include <linux/joystick.h>
#include <linux/input.h>

#include <fcntl.h>

#include <compat/strl.h>
#include <string/stdstring.h>

#include "../input_driver.h"
#include "../../verbosity.h"
#include "../../tasks/tasks_internal.h"

#define NUM_BUTTONS 32
#define NUM_AXES 32
#define H3531_EVENT_MAX 64
#define H3531_BITS_PER_LONG (8U * (unsigned)sizeof(unsigned long))
#define H3531_NBITS(x) (((x) + H3531_BITS_PER_LONG - 1U) / H3531_BITS_PER_LONG)
#define H3531_TEST_BIT(bit, arr) \
   (((arr)[(unsigned)(bit) / H3531_BITS_PER_LONG] >> \
      ((unsigned)(bit) % H3531_BITS_PER_LONG)) & 1UL)

enum h3531_transport
{
   H3531_TRANSPORT_NONE = 0,
   H3531_TRANSPORT_JS,
   H3531_TRANSPORT_EVDEV
};

struct linuxraw_joypad
{
   int fd;
   uint32_t buttons;
   int16_t axes[NUM_AXES];

   char *ident;
   char path[96];
   enum h3531_transport transport;

   uint8_t key_to_button[KEY_MAX + 1];
   uint8_t abs_to_axis[ABS_MAX + 1];
   struct input_absinfo absinfo[NUM_AXES];
   struct input_id inputid;
};

static struct linuxraw_joypad linuxraw_pads[MAX_USERS];
static int linuxraw_epoll = -1;
static int linuxraw_inotify = -1;
static bool linuxraw_hotplug = false;

static bool h3531_path_in_use(const char *path)
{
   unsigned i;
   for (i = 0; i < MAX_USERS; ++i)
      if (linuxraw_pads[i].fd >= 0 &&
          !strcmp(linuxraw_pads[i].path, path))
         return true;
   return false;
}

static int h3531_find_vacant_pad(void)
{
   unsigned i;
   for (i = 0; i < MAX_USERS; ++i)
      if (linuxraw_pads[i].fd < 0)
         return (int)i;
   return -1;
}

static int16_t h3531_scale_abs(const struct input_absinfo *info, int value)
{
   long long range;
   long long scaled;

   if (!info)
      return 0;

   range = (long long)info->maximum - (long long)info->minimum;
   if (range <= 0)
      return 0;

   scaled = ((long long)value - (long long)info->minimum) * 65534LL / range;
   scaled -= 32767LL;

   if (scaled > 32767LL)
      scaled = 32767LL;
   if (scaled < -32767LL)
      scaled = -32767LL;
   return (int16_t)scaled;
}

static bool h3531_evdev_gamepad_capable(int fd)
{
   unsigned long evbits[H3531_NBITS(EV_MAX + 1)];
   unsigned long keybits[H3531_NBITS(KEY_MAX + 1)];
   unsigned long absbits[H3531_NBITS(ABS_MAX + 1)];
   unsigned game_keys = 0;
   unsigned axes = 0;
   int code;

   memset(evbits, 0, sizeof(evbits));
   memset(keybits, 0, sizeof(keybits));
   memset(absbits, 0, sizeof(absbits));

   if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0)
      return false;
   if (!H3531_TEST_BIT(EV_KEY, evbits))
      return false;
   if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0)
      return false;

   for (code = BTN_JOYSTICK; code < KEY_MAX; ++code)
      if (H3531_TEST_BIT(code, keybits))
         ++game_keys;

   if (H3531_TEST_BIT(KEY_UP, keybits)) ++game_keys;
   if (H3531_TEST_BIT(KEY_DOWN, keybits)) ++game_keys;
   if (H3531_TEST_BIT(KEY_LEFT, keybits)) ++game_keys;
   if (H3531_TEST_BIT(KEY_RIGHT, keybits)) ++game_keys;

   if (H3531_TEST_BIT(EV_ABS, evbits) &&
       ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) >= 0)
      for (code = 0; code <= ABS_MAX; ++code)
         if (H3531_TEST_BIT(code, absbits))
            ++axes;

   return game_keys >= 4 || (game_keys >= 2 && axes >= 2);
}

static void h3531_build_evdev_maps(struct linuxraw_joypad *pad, int fd)
{
   unsigned long keybits[H3531_NBITS(KEY_MAX + 1)];
   unsigned long absbits[H3531_NBITS(ABS_MAX + 1)];
   unsigned button = 0;
   unsigned axis = 0;
   int code;

   memset(keybits, 0, sizeof(keybits));
   memset(absbits, 0, sizeof(absbits));
   memset(pad->key_to_button, 0xff, sizeof(pad->key_to_button));
   memset(pad->abs_to_axis, 0xff, sizeof(pad->abs_to_axis));
   memset(pad->absinfo, 0, sizeof(pad->absinfo));

   ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
   ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);

   for (code = BTN_JOYSTICK; code <= KEY_MAX && button < NUM_BUTTONS; ++code)
      if (H3531_TEST_BIT(code, keybits))
         pad->key_to_button[code] = (uint8_t)button++;

   for (code = 0; code < BTN_JOYSTICK && button < NUM_BUTTONS; ++code)
      if (H3531_TEST_BIT(code, keybits))
         pad->key_to_button[code] = (uint8_t)button++;

   for (code = 0; code <= ABS_MAX && axis < NUM_AXES; ++code)
   {
      struct input_absinfo info;
      if (!H3531_TEST_BIT(code, absbits))
         continue;
      if (ioctl(fd, EVIOCGABS(code), &info) < 0)
         continue;
      if (info.maximum <= info.minimum)
         continue;

      pad->abs_to_axis[code] = (uint8_t)axis;
      pad->absinfo[axis] = info;
      pad->axes[axis] = h3531_scale_abs(&info, info.value);
      ++axis;
   }

   RARCH_LOG("[LinuxRaw/H3531] evdev maps: %u buttons, %u axes.\n",
         button, axis);
}

static void linuxraw_poll_js(struct linuxraw_joypad *pad)
{
   struct js_event event;

   while (read(pad->fd, &event, sizeof(event)) == (ssize_t)sizeof(event))
   {
      unsigned type = event.type & ~JS_EVENT_INIT;
      if (type == JS_EVENT_BUTTON && event.number < NUM_BUTTONS)
      {
         if (event.value)
            BIT32_SET(pad->buttons, event.number);
         else
            BIT32_CLEAR(pad->buttons, event.number);
      }
      else if (type == JS_EVENT_AXIS && event.number < NUM_AXES)
         pad->axes[event.number] = event.value;
   }
}

static void linuxraw_poll_evdev(struct linuxraw_joypad *pad)
{
   struct input_event event;

   while (read(pad->fd, &event, sizeof(event)) == (ssize_t)sizeof(event))
   {
      if (event.type == EV_KEY && event.code <= KEY_MAX)
      {
         unsigned idx = pad->key_to_button[event.code];
         if (idx < NUM_BUTTONS)
         {
            if (event.value)
               BIT32_SET(pad->buttons, idx);
            else
               BIT32_CLEAR(pad->buttons, idx);
         }
      }
      else if (event.type == EV_ABS && event.code <= ABS_MAX)
      {
         unsigned idx = pad->abs_to_axis[event.code];
         if (idx < NUM_AXES)
            pad->axes[idx] = h3531_scale_abs(&pad->absinfo[idx], event.value);
      }
   }
}

static void linuxraw_poll_pad(struct linuxraw_joypad *pad)
{
   if (!pad || pad->fd < 0)
      return;
   if (pad->transport == H3531_TRANSPORT_JS)
      linuxraw_poll_js(pad);
   else if (pad->transport == H3531_TRANSPORT_EVDEV)
      linuxraw_poll_evdev(pad);
}

static void h3531_disconnect_pad(unsigned idx)
{
   struct linuxraw_joypad *pad;

   if (idx >= MAX_USERS)
      return;
   pad = &linuxraw_pads[idx];

   if (pad->fd < 0)
      return;

   if (linuxraw_hotplug && pad->ident && *pad->ident)
   {
      input_autoconfigure_disconnect(idx, pad->ident);
      RARCH_LOG("[LinuxRaw/H3531] Disconnected \"%s\".\n", pad->ident);
   }

   epoll_ctl(linuxraw_epoll, EPOLL_CTL_DEL, pad->fd, NULL);
   close(pad->fd);
   pad->fd = -1;
   pad->buttons = 0;
   memset(pad->axes, 0, sizeof(pad->axes));
   pad->transport = H3531_TRANSPORT_NONE;
   pad->path[0] = '\0';
   if (pad->ident)
      *pad->ident = '\0';

   input_autoconfigure_connect(NULL, NULL, NULL, "linuxraw", idx, 0, 0);
}

static bool h3531_add_epoll(struct linuxraw_joypad *pad)
{
   struct epoll_event event;
   memset(&event, 0, sizeof(event));
   event.events = EPOLLIN | EPOLLERR | EPOLLHUP;
   event.data.ptr = pad;
   return epoll_ctl(linuxraw_epoll, EPOLL_CTL_ADD, pad->fd, &event) >= 0;
}

static bool linuxraw_joypad_init_js(const char *path,
      struct linuxraw_joypad *pad)
{
   if (access(path, R_OK) < 0 || pad->fd >= 0)
      return false;

   pad->fd = open(path, O_RDONLY | O_NONBLOCK);
   if (pad->fd < 0)
      return false;

   *pad->ident = '\0';
   ioctl(pad->fd, JSIOCGNAME(input_config_get_device_name_size(0)), pad->ident);

   if (!h3531_add_epoll(pad))
   {
      close(pad->fd);
      pad->fd = -1;
      return false;
   }

   pad->transport = H3531_TRANSPORT_JS;
   strlcpy(pad->path, path, sizeof(pad->path));
   RARCH_LOG("[LinuxRaw] Device name is \"%s\".\n", pad->ident);
   linuxraw_poll_js(pad);
   return true;
}

static bool linuxraw_joypad_init_event(const char *path,
      struct linuxraw_joypad *pad)
{
   int fd;
   char name[NAME_MAX_LENGTH];

   if (access(path, R_OK) < 0 || pad->fd >= 0 || h3531_path_in_use(path))
      return false;

   fd = open(path, O_RDONLY | O_NONBLOCK);
   if (fd < 0)
      return false;

   if (!h3531_evdev_gamepad_capable(fd))
   {
      close(fd);
      return false;
   }

   memset(name, 0, sizeof(name));
   if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
      snprintf(name, sizeof(name), "H3531 event gamepad");

   pad->fd = fd;
   *pad->ident = '\0';
   strlcpy(pad->ident, name, input_config_get_device_name_size(0));
   memset(&pad->inputid, 0, sizeof(pad->inputid));
   ioctl(fd, EVIOCGID, &pad->inputid);

   h3531_build_evdev_maps(pad, fd);

   if (!h3531_add_epoll(pad))
   {
      close(pad->fd);
      pad->fd = -1;
      return false;
   }

   pad->transport = H3531_TRANSPORT_EVDEV;
   strlcpy(pad->path, path, sizeof(pad->path));

   RARCH_LOG("[LinuxRaw/H3531] Event fallback device name is \"%s\" path=%s.\n",
         pad->ident, path);
   linuxraw_poll_evdev(pad);
   return true;
}

static void h3531_connect_pad(unsigned idx)
{
   struct linuxraw_joypad *pad;
   if (idx >= MAX_USERS)
      return;
   pad = &linuxraw_pads[idx];

   input_autoconfigure_connect(
         pad->ident,
         NULL, NULL,
         "linuxraw",
         idx,
         pad->inputid.vendor,
         pad->inputid.product);
}

static void h3531_scan_event_devices(void)
{
   int ev;
   for (ev = 0; ev < H3531_EVENT_MAX; ++ev)
   {
      char path[64];
      int slot;

      snprintf(path, sizeof(path), "/dev/input/event%d", ev);
      if (h3531_path_in_use(path))
         continue;

      slot = h3531_find_vacant_pad();
      if (slot < 0)
         return;

      if (linuxraw_joypad_init_event(path, &linuxraw_pads[slot]))
         h3531_connect_pad((unsigned)slot);
   }
}

static bool h3531_have_any_js(void)
{
   unsigned i;
   for (i = 0; i < MAX_USERS; ++i)
      if (linuxraw_pads[i].fd >= 0 &&
          linuxraw_pads[i].transport == H3531_TRANSPORT_JS)
         return true;
   return false;
}

static const char *linuxraw_joypad_name(unsigned pad)
{
   if (pad >= MAX_USERS)
      return NULL;
   return linuxraw_pads[pad].ident;
}

static void h3531_handle_input_fs_events(void)
{
   unsigned char buf[4096];

   for (;;)
   {
      ssize_t n = read(linuxraw_inotify, buf, sizeof(buf));
      ssize_t off = 0;

      if (n < 0)
      {
         if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return;
         return;
      }
      if (n == 0)
         return;

      while (off + (ssize_t)sizeof(struct inotify_event) <= n)
      {
         struct inotify_event *event = (struct inotify_event*)(buf + off);

         if (event->len)
         {
            if (!strncmp(event->name, "js", 2))
            {
               unsigned idx = (unsigned)strtoul(event->name + 2, NULL, 10);
               char path[96];
               if (idx < MAX_USERS)
               {
                  snprintf(path, sizeof(path), "/dev/input/%s", event->name);

                  if (event->mask & IN_DELETE)
                     h3531_disconnect_pad(idx);
                  else if (event->mask & (IN_CREATE | IN_ATTRIB | IN_MOVED_TO))
                  {
                     if (linuxraw_pads[idx].fd < 0 &&
                         linuxraw_joypad_init_js(path, &linuxraw_pads[idx]))
                        h3531_connect_pad(idx);
                  }
               }
            }
            else if (!strncmp(event->name, "event", 5))
            {
               char path[96];
               unsigned p;
               snprintf(path, sizeof(path), "/dev/input/%s", event->name);

               if (event->mask & (IN_DELETE | IN_MOVED_FROM))
               {
                  for (p = 0; p < MAX_USERS; ++p)
                     if (linuxraw_pads[p].fd >= 0 &&
                         linuxraw_pads[p].transport == H3531_TRANSPORT_EVDEV &&
                         !strcmp(linuxraw_pads[p].path, path))
                        h3531_disconnect_pad(p);
               }
               else if (event->mask & (IN_CREATE | IN_ATTRIB | IN_MOVED_TO))
               {
                  if (!h3531_have_any_js())
                     h3531_scan_event_devices();
               }
            }
         }

         off += (ssize_t)sizeof(struct inotify_event) + event->len;
      }
   }
}

static void linuxraw_joypad_poll(void)
{
   int i, ret;
   struct epoll_event events[MAX_USERS + 1];

retry:
   ret = epoll_wait(linuxraw_epoll, events, MAX_USERS + 1, 0);
   if (ret < 0 && errno == EINTR)
      goto retry;

   for (i = 0; i < ret; ++i)
   {
      struct linuxraw_joypad *ptr = (struct linuxraw_joypad*)events[i].data.ptr;

      if (ptr)
      {
         if (events[i].events & (EPOLLERR | EPOLLHUP))
         {
            unsigned idx = (unsigned)(ptr - linuxraw_pads);
            h3531_disconnect_pad(idx);
         }
         else
            linuxraw_poll_pad(ptr);
      }
      else
         h3531_handle_input_fs_events();
   }
}

static void *linuxraw_joypad_init(void *data)
{
   size_t i;
   char path[PATH_MAX_LENGTH];
   bool any_js = false;

   (void)data;

   linuxraw_epoll = epoll_create(32);
   if (linuxraw_epoll < 0)
      return NULL;

   for (i = 0; i < MAX_USERS; ++i)
   {
      struct linuxraw_joypad *pad = &linuxraw_pads[i];

      memset(pad, 0, sizeof(*pad));
      pad->fd = -1;
      pad->ident = input_config_get_device_name_ptr(i);
      memset(pad->key_to_button, 0xff, sizeof(pad->key_to_button));
      memset(pad->abs_to_axis, 0xff, sizeof(pad->abs_to_axis));

      snprintf(path, sizeof(path), "/dev/input/js%u", (unsigned)i);
      if (linuxraw_joypad_init_js(path, pad))
      {
         any_js = true;
         input_autoconfigure_connect(
               pad->ident, NULL, NULL, "linuxraw", (unsigned)i, 0, 0);
      }
   }

   if (!any_js)
   {
      RARCH_LOG("[LinuxRaw/H3531] /dev/input/js* absent; enabling event fallback.\n");
      h3531_scan_event_devices();
   }

   linuxraw_inotify = inotify_init();
   if (linuxraw_inotify >= 0)
   {
      struct epoll_event event;
      fcntl(linuxraw_inotify, F_SETFL,
            fcntl(linuxraw_inotify, F_GETFL) | O_NONBLOCK);
      inotify_add_watch(linuxraw_inotify, "/dev/input",
            IN_DELETE | IN_CREATE | IN_ATTRIB | IN_MOVED_TO | IN_MOVED_FROM);

      memset(&event, 0, sizeof(event));
      event.events = EPOLLIN;
      event.data.ptr = NULL;
      if (epoll_ctl(linuxraw_epoll, EPOLL_CTL_ADD, linuxraw_inotify, &event) < 0)
         RARCH_ERR("[LinuxRaw/H3531] Failed to add inotify FD: %s.\n",
               strerror(errno));
   }

   linuxraw_hotplug = true;
   return (void*)-1;
}

static void linuxraw_joypad_destroy(void)
{
   unsigned i;

   for (i = 0; i < MAX_USERS; ++i)
      if (linuxraw_pads[i].fd >= 0)
         close(linuxraw_pads[i].fd);

   memset(linuxraw_pads, 0, sizeof(linuxraw_pads));
   for (i = 0; i < MAX_USERS; ++i)
      linuxraw_pads[i].fd = -1;

   if (linuxraw_inotify >= 0)
      close(linuxraw_inotify);
   linuxraw_inotify = -1;

   if (linuxraw_epoll >= 0)
      close(linuxraw_epoll);
   linuxraw_epoll = -1;

   linuxraw_hotplug = false;
}

static int32_t linuxraw_joypad_button(unsigned port, uint16_t joykey)
{
   const struct linuxraw_joypad *pad;
   if (port >= MAX_USERS)
      return 0;
   pad = &linuxraw_pads[port];
   if (joykey < NUM_BUTTONS)
      return BIT32_GET(pad->buttons, joykey);
   return 0;
}

static void linuxraw_joypad_get_buttons(unsigned port, input_bits_t *state)
{
   const struct linuxraw_joypad *pad =
      port < MAX_USERS ? &linuxraw_pads[port] : NULL;

   if (pad)
      BITS_COPY16_PTR(state, pad->buttons);
   else
      BIT256_CLEAR_ALL_PTR(state);
}

static int16_t linuxraw_joypad_axis_state(
      const struct linuxraw_joypad *pad,
      unsigned port, uint32_t joyaxis)
{
   (void)port;

   if (AXIS_NEG_GET(joyaxis) < NUM_AXES)
   {
      int16_t val = pad->axes[AXIS_NEG_GET(joyaxis)];
      if (val < 0)
         return val;
   }
   else if (AXIS_POS_GET(joyaxis) < NUM_AXES)
   {
      int16_t val = pad->axes[AXIS_POS_GET(joyaxis)];
      if (val > 0)
         return val;
   }
   return 0;
}

static int16_t linuxraw_joypad_axis(unsigned port, uint32_t joyaxis)
{
   if (port >= MAX_USERS)
      return 0;
   return linuxraw_joypad_axis_state(&linuxraw_pads[port], port, joyaxis);
}

static int16_t linuxraw_joypad_state(
      rarch_joypad_info_t *joypad_info,
      const struct retro_keybind *binds,
      unsigned port)
{
   int i;
   int16_t ret = 0;
   uint16_t port_idx;
   const struct linuxraw_joypad *pad;

   if (!joypad_info)
      return 0;

   port_idx = joypad_info->joy_idx;
   if (port_idx >= MAX_USERS)
      return 0;
   pad = &linuxraw_pads[port_idx];

   for (i = 0; i < RARCH_FIRST_CUSTOM_BIND; ++i)
   {
      const uint64_t joykey = (binds[i].joykey != NO_BTN)
         ? binds[i].joykey : joypad_info->auto_binds[i].joykey;
      const uint32_t joyaxis = (binds[i].joyaxis != AXIS_NONE)
         ? binds[i].joyaxis : joypad_info->auto_binds[i].joyaxis;

      if ((uint16_t)joykey != NO_BTN &&
          joykey < NUM_BUTTONS &&
          BIT32_GET(pad->buttons, joykey))
         ret |= (1 << i);
      else if (joyaxis != AXIS_NONE &&
               ((float)abs(linuxraw_joypad_axis_state(
                    pad, port_idx, joyaxis)) / 0x8000) >
                    joypad_info->axis_threshold)
         ret |= (1 << i);
   }

   return ret;
}

static bool linuxraw_joypad_query_pad(unsigned pad)
{
   return pad < MAX_USERS && linuxraw_pads[pad].fd >= 0;
}

input_device_driver_t linuxraw_joypad = {
   linuxraw_joypad_init,
   linuxraw_joypad_query_pad,
   linuxraw_joypad_destroy,
   linuxraw_joypad_button,
   linuxraw_joypad_state,
   linuxraw_joypad_get_buttons,
   linuxraw_joypad_axis,
   linuxraw_joypad_poll,
   NULL,
   NULL,
   NULL,
   NULL,
   linuxraw_joypad_name,
   "linuxraw",
};
