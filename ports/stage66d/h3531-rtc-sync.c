#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/rtc.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define H3531_RTC_MIN_YEAR 2020
#define H3531_RTC_MAX_YEAR 2099
#define H3531_RTC_SYNC_THRESHOLD 120
#define H3531_NTP_UNIX_DELTA 2208988800UL

static volatile sig_atomic_t running = 1;

static void on_signal(int sig)
{
   (void)sig;
   running = 0;
}

static int open_rtc(const char **path_out)
{
   static const char *paths[] = {
      "/dev/rtc0", "/dev/rtc", "/dev/misc/rtc", "/dev/rtc1"
   };
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

static void print_system(const char *prefix, time_t t)
{
   struct tm tmv;
   char buf[64];
   if (!gmtime_r(&t, &tmv)) return;
   strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tmv);
   printf("[RTC] %s %s\n", prefix, buf);
   fflush(stdout);
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

static int persist_system_to_rtc(void)
{
   const char *path = NULL;
   int fd = open_rtc(&path);
   int rc;
   time_t now = time(NULL);

   if (fd < 0)
   {
      printf("[RTC] persist skipped: no writable RTC device\n");
      return 2;
   }

   printf("[RTC] persist device=%s\n", path);
   rc = write_rtc(fd, now);
   if (rc < 0)
      printf("[RTC] RTC_SET_TIME failed errno=%d (%s)\n", errno, strerror(errno));
   close(fd);
   return rc < 0 ? 3 : 0;
}

static int set_system_epoch(time_t t, const char *source)
{
   struct timespec ts;

   if (!system_valid(t))
   {
      printf("[RTC] refusing invalid system time from %s\n", source ? source : "unknown");
      return 4;
   }

   ts.tv_sec = t;
   ts.tv_nsec = 0;
   if (clock_settime(CLOCK_REALTIME, &ts) < 0)
   {
      printf("[RTC] clock_settime failed errno=%d (%s)\n", errno, strerror(errno));
      return 5;
   }

   printf("[RTC] system clock set source=%s\n", source ? source : "unknown");
   print_system("system", t);
   persist_system_to_rtc();
   return 0;
}

static int load_rtc(void)
{
   const char *path = NULL;
   struct rtc_time rt;
   time_t t;
   int fd = open_rtc(&path);

   if (fd < 0)
   {
      printf("[RTC] no /dev/rtc0 /dev/rtc /dev/misc/rtc /dev/rtc1 device; battery clock is not exposed to Linux\n");
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
   close(fd);

   if (!rtc_valid(&rt) || rtc_to_epoch(&rt, &t) != 0)
   {
      printf("[RTC] hardware clock invalid/uninitialized; waiting for network or manual time\n");
      return 4;
   }

   return set_system_epoch(t, "battery-rtc");
}

static uint32_t read_be32(const unsigned char *p)
{
   return ((uint32_t)p[0] << 24) |
          ((uint32_t)p[1] << 16) |
          ((uint32_t)p[2] << 8) |
          (uint32_t)p[3];
}

static int ntp_sync_host(const char *host)
{
   struct addrinfo hints, *res = NULL, *ai;
   int gai;
   int rc = 1;

   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_DGRAM;

   gai = getaddrinfo(host, "123", &hints, &res);
   if (gai != 0)
   {
      printf("[RTC] NTP DNS failed host=%s error=%s\n", host, gai_strerror(gai));
      return 2;
   }

   for (ai = res; ai; ai = ai->ai_next)
   {
      unsigned char packet[48];
      struct timeval tv;
      ssize_t n;
      int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd < 0)
         continue;

      tv.tv_sec = 3;
      tv.tv_usec = 0;
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

      memset(packet, 0, sizeof(packet));
      packet[0] = 0x23; /* LI=0, VN=4, mode=3 client */

      if (sendto(fd, packet, sizeof(packet), 0,
               ai->ai_addr, ai->ai_addrlen) != (ssize_t)sizeof(packet))
      {
         close(fd);
         continue;
      }

      n = recvfrom(fd, packet, sizeof(packet), 0, NULL, NULL);
      close(fd);

      if (n >= 48)
      {
         uint32_t ntp_sec = read_be32(packet + 40);
         if (ntp_sec > H3531_NTP_UNIX_DELTA)
         {
            time_t unix_sec = (time_t)(ntp_sec - H3531_NTP_UNIX_DELTA);
            if (system_valid(unix_sec))
            {
               printf("[RTC] NTP reply host=%s\n", host);
               rc = set_system_epoch(unix_sec, host);
               break;
            }
         }
      }
   }

   freeaddrinfo(res);
   if (rc != 0)
      printf("[RTC] NTP sync failed host=%s\n", host);
   return rc;
}

static int ntp_sync(void)
{
   static const char *hosts[] = {
      "time.cloudflare.com",
      "pool.ntp.org",
      "time.google.com"
   };
   size_t i;
   for (i = 0; i < sizeof(hosts)/sizeof(hosts[0]); ++i)
   {
      printf("[RTC] NTP trying host=%s\n", hosts[i]);
      fflush(stdout);
      if (ntp_sync_host(hosts[i]) == 0)
         return 0;
   }
   return 2;
}

static int manual_set(const char *date_text, const char *time_text)
{
   int y, mon, d, h, min, sec;
   struct tm tmv;
   time_t t;

   if (!date_text || !time_text ||
       sscanf(date_text, "%d-%d-%d", &y, &mon, &d) != 3 ||
       sscanf(time_text, "%d:%d:%d", &h, &min, &sec) != 3)
   {
      printf("[RTC] manual format must be YYYY-MM-DD HH:MM:SS\n");
      return 64;
   }

   if (y < H3531_RTC_MIN_YEAR || y > H3531_RTC_MAX_YEAR ||
       mon < 1 || mon > 12 || d < 1 || d > 31 ||
       h < 0 || h > 23 || min < 0 || min > 59 || sec < 0 || sec > 59)
   {
      printf("[RTC] manual date/time out of range\n");
      return 65;
   }

   memset(&tmv, 0, sizeof(tmv));
   tmv.tm_year = y - 1900;
   tmv.tm_mon = mon - 1;
   tmv.tm_mday = d;
   tmv.tm_hour = h;
   tmv.tm_min = min;
   tmv.tm_sec = sec;
   tmv.tm_isdst = -1;

   /* The current embedded desktop has no timezone database configured;
    * treat entered wall-clock values as the system clock value directly. */
   t = timegm(&tmv);
   if (t == (time_t)-1)
      return 66;

   return set_system_epoch(t, "manual");
}

static int status_rtc(void)
{
   const char *path = NULL;
   int fd;
   struct rtc_time rt;
   time_t now = time(NULL);

   print_system("system-now", now);

   fd = open_rtc(&path);
   if (fd < 0)
   {
      printf("[RTC] status: no writable RTC device found\n");
      return 2;
   }

   printf("[RTC] status device=%s\n", path);
   if (read_rtc(fd, &rt) == 0)
      print_rtc("status-rtc", &rt);
   else
      printf("[RTC] status RTC_RD_TIME failed errno=%d (%s)\n", errno, strerror(errno));
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
      return persist_system_to_rtc();
   if (strcmp(argv[1], "net") == 0)
      return ntp_sync();
   if (strcmp(argv[1], "status") == 0)
      return status_rtc();
   if (strcmp(argv[1], "set") == 0 && argc == 4)
      return manual_set(argv[2], argv[3]);

   fprintf(stderr,
      "usage: %s [load|watch|save|net|status|set YYYY-MM-DD HH:MM:SS]\n",
      argv[0]);
   return 64;
}
