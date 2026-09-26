#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/rtc.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define H3531_RTC_MIN_YEAR 2020
#define H3531_RTC_MAX_YEAR 2099
#define H3531_RTC_SYNC_THRESHOLD 120

static volatile sig_atomic_t running = 1;

static void on_signal(int sig)
{
   (void)sig;
   running = 0;
}

static int open_rtc(const char **path_out)
{
   static const char *paths[] = {"/dev/rtc0", "/dev/rtc"};
   size_t i;
   for (i = 0; i < sizeof(paths)/sizeof(paths[0]); ++i)
   {
      int fd = open(paths[i], O_RDWR);
      if (fd >= 0)
      {
         if (path_out) *path_out = paths[i];
         return fd;
      }
   }
   return -1;
}

static int rtc_valid(const struct rtc_time *rt)
{
   int year;
   if (!rt) return 0;
   year = rt->tm_year + 1900;
   if (year < H3531_RTC_MIN_YEAR || year > H3531_RTC_MAX_YEAR) return 0;
   if (rt->tm_mon < 0 || rt->tm_mon > 11) return 0;
   if (rt->tm_mday < 1 || rt->tm_mday > 31) return 0;
   if (rt->tm_hour < 0 || rt->tm_hour > 23) return 0;
   if (rt->tm_min < 0 || rt->tm_min > 59) return 0;
   if (rt->tm_sec < 0 || rt->tm_sec > 60) return 0;
   return 1;
}

static int system_valid(time_t t)
{
   struct tm tmv;
   if (!gmtime_r(&t, &tmv)) return 0;
   return tmv.tm_year + 1900 >= H3531_RTC_MIN_YEAR &&
          tmv.tm_year + 1900 <= H3531_RTC_MAX_YEAR;
}

static void print_rtc(const char *prefix, const struct rtc_time *rt)
{
   if (!rt) return;
   printf("[RTC] %s %04d-%02d-%02d %02d:%02d:%02d UTC\n",
         prefix,
         rt->tm_year + 1900, rt->tm_mon + 1, rt->tm_mday,
         rt->tm_hour, rt->tm_min, rt->tm_sec);
   fflush(stdout);
}

static int rtc_to_epoch(const struct rtc_time *rt, time_t *out)
{
   struct tm tmv;
   time_t t;
   if (!rtc_valid(rt) || !out) return -1;
   memset(&tmv, 0, sizeof(tmv));
   tmv.tm_sec = rt->tm_sec;
   tmv.tm_min = rt->tm_min;
   tmv.tm_hour = rt->tm_hour;
   tmv.tm_mday = rt->tm_mday;
   tmv.tm_mon = rt->tm_mon;
   tmv.tm_year = rt->tm_year;
   tmv.tm_isdst = 0;
   t = timegm(&tmv);
   if (t == (time_t)-1) return -1;
   *out = t;
   return 0;
}

static int read_rtc(int fd, struct rtc_time *rt)
{
   memset(rt, 0, sizeof(*rt));
   if (ioctl(fd, RTC_RD_TIME, rt) < 0)
      return -1;
   return 0;
}

static int write_rtc(int fd, time_t now)
{
   struct tm tmv;
   struct rtc_time rt;
   if (!system_valid(now) || !gmtime_r(&now, &tmv))
      return -1;
   memset(&rt, 0, sizeof(rt));
   rt.tm_sec = tmv.tm_sec;
   rt.tm_min = tmv.tm_min;
   rt.tm_hour = tmv.tm_hour;
   rt.tm_mday = tmv.tm_mday;
   rt.tm_mon = tmv.tm_mon;
   rt.tm_year = tmv.tm_year;
   rt.tm_wday = tmv.tm_wday;
   rt.tm_yday = tmv.tm_yday;
   rt.tm_isdst = 0;
   if (ioctl(fd, RTC_SET_TIME, &rt) < 0)
      return -1;
   print_rtc("written", &rt);
   return 0;
}

static int load_rtc(void)
{
   const char *path = NULL;
   struct rtc_time rt;
   time_t t;
   struct timespec ts;
   int fd = open_rtc(&path);
   if (fd < 0)
   {
      printf("[RTC] no /dev/rtc0 or /dev/rtc device; battery clock is not exposed to Linux\n");
      return 2;
   }

   printf("[RTC] device=%s\n", path);
   if (read_rtc(fd, &rt) < 0)
   {
      printf("[RTC] RTC_RD_TIME failed errno=%d (%s)\n", errno, strerror(errno));
      close(fd);
      return 3;
   }
   print_rtc("read", &rt);

   if (!rtc_valid(&rt) || rtc_to_epoch(&rt, &t) != 0)
   {
      printf("[RTC] hardware clock invalid; system clock left unchanged\n");
      close(fd);
      return 4;
   }

   ts.tv_sec = t;
   ts.tv_nsec = 0;
   if (clock_settime(CLOCK_REALTIME, &ts) < 0)
   {
      printf("[RTC] clock_settime failed errno=%d (%s)\n", errno, strerror(errno));
      close(fd);
      return 5;
   }

   printf("[RTC] system clock loaded from battery RTC\n");
   close(fd);
   return 0;
}

static int watch_rtc(void)
{
   const char *path = NULL;
   int fd;
   signal(SIGTERM, on_signal);
   signal(SIGINT, on_signal);
   signal(SIGHUP, on_signal);

   fd = open_rtc(&path);
   if (fd < 0)
   {
      printf("[RTC] watch unavailable: no RTC device\n");
      return 2;
   }
   printf("[RTC] watch active device=%s threshold=%ds\n",
         path, H3531_RTC_SYNC_THRESHOLD);
   fflush(stdout);

   while (running)
   {
      struct rtc_time rt;
      time_t now = time(NULL);
      time_t rtc_epoch = (time_t)-1;
      int have_rtc = read_rtc(fd, &rt) == 0 && rtc_valid(&rt) &&
                     rtc_to_epoch(&rt, &rtc_epoch) == 0;
      int have_sys = system_valid(now);

      if (have_sys && !have_rtc)
      {
         printf("[RTC] system time valid but RTC invalid; seeding battery clock\n");
         if (write_rtc(fd, now) < 0)
            printf("[RTC] RTC_SET_TIME failed errno=%d (%s)\n", errno, strerror(errno));
      }
      else if (!have_sys && have_rtc)
      {
         struct timespec ts;
         ts.tv_sec = rtc_epoch;
         ts.tv_nsec = 0;
         if (clock_settime(CLOCK_REALTIME, &ts) == 0)
            printf("[RTC] invalid system clock recovered from RTC\n");
      }
      else if (have_sys && have_rtc)
      {
         long long delta = (long long)now - (long long)rtc_epoch;
         if (delta < 0) delta = -delta;
         if (delta > H3531_RTC_SYNC_THRESHOLD)
         {
            printf("[RTC] system/RTC delta=%llds; persisting corrected system time\n", delta);
            if (write_rtc(fd, now) < 0)
               printf("[RTC] RTC_SET_TIME failed errno=%d (%s)\n", errno, strerror(errno));
         }
      }
      fflush(stdout);

      for (int i = 0; i < 30 && running; ++i)
         sleep(1);
   }

   close(fd);
   printf("[RTC] watch stopped\n");
   return 0;
}

int main(int argc, char **argv)
{
   if (argc < 2 || strcmp(argv[1], "load") == 0)
      return load_rtc();
   if (strcmp(argv[1], "watch") == 0)
      return watch_rtc();
   if (strcmp(argv[1], "save") == 0)
   {
      const char *path = NULL;
      int fd = open_rtc(&path);
      time_t now = time(NULL);
      int rc;
      if (fd < 0)
      {
         printf("[RTC] save unavailable: no RTC device\n");
         return 2;
      }
      printf("[RTC] save device=%s\n", path);
      rc = write_rtc(fd, now);
      if (rc < 0)
      {
         printf("[RTC] RTC_SET_TIME failed errno=%d (%s)\n", errno, strerror(errno));
         close(fd);
         return 3;
      }
      close(fd);
      return 0;
   }

   fprintf(stderr, "usage: %s [load|watch|save]\n", argv[0]);
   return 64;
}
