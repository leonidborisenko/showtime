/*
 *  Copyright (C) 2007-2015 Lonelycoder AB
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  This program is also available under a commercial proprietary license.
 *  For more information, contact andreas@lonelycoder.com
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <locale.h>
#include "arch/arch.h"
#include <limits.h>
#include <syslog.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <signal.h>
#include "text/text.h"
#include "main.h"

#include "networking/net.h"

#include "posix.h"

#include <sys/utsname.h>

static int decorate_trace;

#ifdef linux

/**
 *
 */
static char *
linux_get_dist(void)
{
  char buf[1024] = {0};
  FILE *fp = popen("lsb_release -d", "r");
  if(fp == NULL)
    return NULL;

  char *ret = NULL;
  while(1) {
    int r = fread(buf, 1, sizeof(buf) - 1, fp);
    if(r == 0)
      break;

    const char *s;
    if((s = mystrbegins(buf, "Description:")) != NULL) {
      while(*s && *s <= 32)
        s++;

      if(*s) {
        ret = strdup(s);
        ret[strcspn(ret, "\n\r")] = 0;
        break;
      }
    }
  }

  fclose(fp);
  return ret;
}
#endif


/**
 *
 */
void
posix_init(void)
{
  struct utsname uts;

  if(!uname(&uts)) {
    char *dist = NULL;
#ifdef linux
    dist = linux_get_dist();
#endif
    if(dist != NULL) {
      snprintf(gconf.os_info, sizeof(gconf.os_info), "%s", dist);
      free(dist);
    } else {
      snprintf(gconf.os_info, sizeof(gconf.os_info),
               "%s-%s-%s-%s",
               uts.sysname,
               uts.release,
               uts.version,
               uts.machine);
    }
  }

  struct timeval tv;
  gettimeofday(&tv, NULL);
  srand(tv.tv_usec);

#ifdef STOS
  gconf.cache_path = strdup("/stos/cache/showtime");
  gconf.persistent_path = strdup("/stos/persistent/showtime");
#else
  const char *homedir = getenv("HOME");

  if(homedir != NULL) {
    char buf[PATH_MAX];

    snprintf(buf, sizeof(buf), "%s/.cache/showtime", homedir);
    gconf.cache_path = strdup(buf);

    snprintf(buf, sizeof(buf), "%s/.hts/showtime", homedir);
    gconf.persistent_path = strdup(buf);
  }
#endif

  setlocale(LC_ALL, "");
  decorate_trace = isatty(2);

  signal(SIGPIPE, SIG_IGN);
  
  TRACE(TRACE_INFO, "core", "Using %d CPU(s)", gconf.concurrency);

#ifdef RLIMIT_AS
  do {
    struct rlimit rlim;
    getrlimit(RLIMIT_AS, &rlim);
    rlim.rlim_cur = 512 * 1024 * 1024;
    setrlimit(RLIMIT_AS, &rlim);
  } while(0);
#endif

#ifdef RLIMIT_DATA
  do {
    struct rlimit rlim;
    getrlimit(RLIMIT_DATA, &rlim);
    rlim.rlim_cur = 512 * 1024 * 1024;
    setrlimit(RLIMIT_DATA, &rlim);
  } while(0);
#endif

  if(gconf.trace_to_syslog)
    openlog("showtime", 0, LOG_USER);
}


/**
 *
 */
void
trace_arch(int level, const char *prefix, const char *str)
{
  const char *sgr, *sgroff;
  int prio = LOG_ERR;

  switch(level) {
  case TRACE_EMERG: sgr = "\033[31m"; prio = LOG_ERR;   break;
  case TRACE_ERROR: sgr = "\033[31m"; prio = LOG_ERR;   break;
  case TRACE_INFO:  sgr = "\033[33m"; prio = LOG_INFO;  break;
  case TRACE_DEBUG: sgr = "\033[32m"; prio = LOG_DEBUG; break;
  default:          sgr = "\033[35m"; break;
  }

  if(!decorate_trace) {
    sgr = "";
    sgroff = "";
  } else {
    sgroff = "\033[0m";
  }

  fprintf(stderr, "%s%s %s%s\n", sgr, prefix, str, sgroff);

  if(gconf.trace_to_syslog)
    syslog(prio, "%s %s", prefix, str);
}


/**
 *
 */
int64_t
arch_get_ts(void)
{
#if _POSIX_TIMERS > 0 && defined(_POSIX_MONOTONIC_CLOCK)
  struct timespec tv;
  clock_gettime(CLOCK_MONOTONIC, &tv);
  return (int64_t)tv.tv_sec * 1000000LL + (tv.tv_nsec / 1000);
#else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
#endif
}


/**
 *
 */
uint64_t
arch_get_seed(void)
{
  uint64_t v = getpid();
  v = (v << 16) ^ getppid();
  v = (v << 32) ^ time(NULL);
  return v;
}


#include <sys/mman.h>
#include "arch/halloc.h"

/**
 *
 */
void *
halloc(size_t size)
{
  void *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if(p == MAP_FAILED)
    return NULL;
  return p;
}

/**
 *
 */
void
hfree(void *ptr, size_t size)
{
  munmap(ptr, size);
}


void
arch_localtime(const time_t *now, struct tm *tm)
{
  localtime_r(now, tm);
}


int
arch_pipe(int pipefd[2])
{
  return pipe(pipefd);
}

void *
mymalloc(size_t size)
{
  return malloc(size);
}

void *
myrealloc(void *ptr, size_t size)
{
  return realloc(ptr, size);
}

void *
mycalloc(size_t count, size_t size)
{
  return calloc(count, size);
}

void *
mymemalign(size_t align, size_t size)
{
  void *p;
  return posix_memalign(&p, align, size) ? NULL : p;
}

