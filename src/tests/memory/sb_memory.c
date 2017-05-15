/* Copyright (C) 2004 MySQL AB
   Copyright (C) 2004-2017 Alexey Kopytov <akopytov@gmail.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#ifdef _WIN32
#include "sb_win.h"
#endif

#include <errno.h>
#include <sched.h>
#include "sysbench.h"
#include "sb_rand.h"

#ifdef HAVE_SYS_IPC_H
# include <sys/ipc.h>
#endif

#ifdef HAVE_SYS_SHM_H
# include <sys/shm.h>
#endif

#include <inttypes.h>

#define LARGE_PAGE_SIZE (4UL * 1024 * 1024)

/* Memory test arguments */
static sb_arg_t memory_args[] =
{
  SB_OPT("memory-block-size", "size of memory block for test", "1K", SIZE),
  SB_OPT("memory-total-size", "total size of data to transfer", "100G", SIZE),
  SB_OPT("memory-scope", "memory access scope {global,local}", "global",
         STRING),
#ifdef HAVE_LARGE_PAGES
  SB_OPT("memory-hugetlb", "allocate memory from HugeTLB pool", "off", BOOL),
#endif
  SB_OPT("memory-oper", "type of memory operations {read, write, none}",
         "write", STRING),
  SB_OPT("memory-access-mode", "memory access mode {seq,rnd}", "seq", STRING),

  SB_OPT_END
};

/* Memory test operations */
static int memory_init(void);
static int memory_thread_init(int);
static void memory_print_mode(void);
static sb_event_t memory_next_event(int);
static int event_rnd_none(sb_event_t *, int);
static int event_rnd_read(sb_event_t *, int);
static int event_rnd_write(sb_event_t *, int);
static int event_seq_none(sb_event_t *, int);
static int event_seq_read(sb_event_t *, int);
static int event_seq_write(sb_event_t *, int);
static void memory_report_intermediate(sb_stat_t *);
static void memory_report_cumulative(sb_stat_t *);

static sb_test_t memory_test =
{
  .sname = "memory",
  .lname = "Memory functions speed test",
  .ops = {
    .init = memory_init,
    .thread_init = memory_thread_init,
    .print_mode = memory_print_mode,
    .next_event = memory_next_event,
    .report_intermediate = memory_report_intermediate,
    .report_cumulative = memory_report_cumulative
  },
  .args = memory_args
};

/* Test arguments */

static unsigned long *per_exec_times;
static unsigned long *per_exec_times_min;
static unsigned long *per_exec_times_max;
static unsigned long *per_exec_times_cnt;
static unsigned long *per_exec_times_miss;
static ssize_t memory_block_size;
static long long    memory_total_size;
static unsigned int memory_scope;
static unsigned int memory_oper;
static unsigned int memory_access_rnd;
#ifdef HAVE_LARGE_PAGES
static unsigned int memory_hugetlb;
#endif

static TLS uint64_t tls_total_ops CK_CC_CACHELINE;
static TLS size_t *tls_buf;
static TLS size_t *tls_buf_end;

/* Array of per-thread buffers */
static size_t **buffers;
/* Global buffer */
static size_t *buffer;

static void diff_timespec(int thread_id, const struct timespec *old, const struct timespec *new, struct timespec *diff)
{
	if (new->tv_sec == old->tv_sec && new->tv_nsec == old->tv_nsec)
		log_text(LOG_FATAL, "%s: #%d time did not move: %ld/%ld == %ld/%ld", __func__, thread_id, old->tv_sec, old->tv_nsec, new->tv_sec, new->tv_nsec);
	if ( (new->tv_sec < old->tv_sec) || (new->tv_sec == old->tv_sec && new->tv_nsec < old->tv_nsec)	)
		log_text(LOG_FATAL, "%s: #%d time went backwards: %ld/%ld -> %ld/%ld", __func__, thread_id, old->tv_sec, old->tv_nsec, new->tv_sec, new->tv_nsec);
	if ((new->tv_nsec - old->tv_nsec) < 0) {
		diff->tv_sec = new->tv_sec - old->tv_sec - 1;
		diff->tv_nsec = new->tv_nsec - old->tv_nsec + 1000000000;
	} else {
		diff->tv_sec = new->tv_sec - old->tv_sec;
		diff->tv_nsec = new->tv_nsec - old->tv_nsec;
	}
	if (diff->tv_sec < 0)
		log_text(LOG_FATAL, "%s: #%d time diff broken. old: %ld/%ld new: %ld/%ld diff: %ld/%ld ", __func__, thread_id, old->tv_sec, old->tv_nsec, new->tv_sec, new->tv_nsec, diff->tv_sec, diff->tv_nsec);
}


#ifdef HAVE_LARGE_PAGES
static void * hugetlb_alloc(size_t size);
#endif

int register_test_memory(sb_list_t *tests)
{
  SB_LIST_ADD_TAIL(&memory_test.listitem, tests);

  return 0;
}


int memory_init(void)
{
  unsigned int i;
  char         *s;

  memory_block_size = sb_get_value_size("memory-block-size");
  if (memory_block_size < SIZEOF_SIZE_T ||
      /* Must be a power of 2 */
      (memory_block_size & (memory_block_size - 1)) != 0)
  {
    log_text(LOG_FATAL, "Invalid value for memory-block-size: %s",
             sb_get_value_string("memory-block-size"));
    return 1;
  }

  memory_total_size = sb_get_value_size("memory-total-size");

  s = sb_get_value_string("memory-scope");
  if (!strcmp(s, "global"))
    memory_scope = SB_MEM_SCOPE_GLOBAL;
  else if (!strcmp(s, "local"))
    memory_scope = SB_MEM_SCOPE_LOCAL;
  else
  {
    log_text(LOG_FATAL, "Invalid value for memory-scope: %s", s);
    return 1;
  }

#ifdef HAVE_LARGE_PAGES
    memory_hugetlb = sb_get_value_flag("memory-hugetlb");
#endif  

  s = sb_get_value_string("memory-oper");
  if (!strcmp(s, "write"))
    memory_oper = SB_MEM_OP_WRITE;
  else if (!strcmp(s, "read"))
    memory_oper = SB_MEM_OP_READ;
  else if (!strcmp(s, "none"))
    memory_oper = SB_MEM_OP_NONE;
  else
  {
    log_text(LOG_FATAL, "Invalid value for memory-oper: %s", s);
    return 1;
  }

  s = sb_get_value_string("memory-access-mode");
  if (!strcmp(s, "seq"))
    memory_access_rnd = 0;
  else if (!strcmp(s, "rnd"))
    memory_access_rnd = 1;
  else
  {
    log_text(LOG_FATAL, "Invalid value for memory-access-mode: %s", s);
    return 1;
  }
  
  if (memory_scope == SB_MEM_SCOPE_GLOBAL)
  {
#ifdef HAVE_LARGE_PAGES
    if (memory_hugetlb)
      buffer = hugetlb_alloc(memory_block_size);
    else
#endif
      buffer = sb_memalign(memory_block_size, sb_getpagesize());

    if (buffer == NULL)
    {
      log_text(LOG_FATAL, "Failed to allocate buffer!");
      return 1;
    }

    memset(buffer, 0, memory_block_size);
  }
  else
  {
    buffers = malloc(sb_globals.threads * sizeof(void *));
    if (buffers == NULL)
    {
      log_text(LOG_FATAL, "Failed to allocate buffers array!");
      return 1;
    }
    for (i = 0; i < sb_globals.threads; i++)
    {
#ifdef HAVE_LARGE_PAGES
      if (memory_hugetlb)
        buffers[i] = hugetlb_alloc(memory_block_size);
      else
#endif
        buffers[i] = sb_memalign(memory_block_size, sb_getpagesize());

      if (buffers[i] == NULL)
      {
        log_text(LOG_FATAL, "Failed to allocate buffer for thread #%d!", i);
        return 1;
      }

      memset(buffers[i], 0, memory_block_size);
    }
  }

  switch (memory_oper) {
  case SB_MEM_OP_NONE:
    memory_test.ops.execute_event =
      memory_access_rnd ? event_rnd_none : event_seq_none;
    break;

  case SB_MEM_OP_READ:
    memory_test.ops.execute_event =
      memory_access_rnd ? event_rnd_read : event_seq_read;
    break;

  case SB_MEM_OP_WRITE:
    memory_test.ops.execute_event =
      memory_access_rnd ? event_rnd_write : event_seq_write;
    break;

  default:
    log_text(LOG_FATAL, "Unknown memory request type: %d\n", memory_oper);
    return 1;
  }

  /* Use our own limit on the number of events */
  sb_globals.max_events = 0;

  per_exec_times = calloc(sb_globals.threads, sizeof(*per_exec_times));
  per_exec_times_min = calloc(sb_globals.threads, sizeof(*per_exec_times_min));
  per_exec_times_max = calloc(sb_globals.threads, sizeof(*per_exec_times_max));
  per_exec_times_cnt = calloc(sb_globals.threads, sizeof(*per_exec_times_cnt));
  per_exec_times_miss = calloc(sb_globals.threads, sizeof(*per_exec_times_miss));

  return 0;
}


int memory_thread_init(int thread_id)
{
  (void) thread_id; /* unused */

  cpu_set_t set;
  int rc;
  char *e;

  e = calloc(123, 1);
  CPU_ZERO(&set);
  CPU_SET(thread_id, &set);
  rc = sched_setaffinity(0, sizeof(set), &set);
  if (strerror_r(errno, e, 123))
	  perror("strerror");
  log_text(LOG_INFO, "thread %d %s", thread_id, e);
  free(e);

  /* Initialize thread-local variables for each thread */

  if (memory_total_size > 0)
  {
    tls_total_ops = memory_total_size / memory_block_size / sb_globals.threads;
  }

  switch (memory_scope) {
  case SB_MEM_SCOPE_GLOBAL:
    tls_buf = buffer;
    break;
  case SB_MEM_SCOPE_LOCAL:
    tls_buf = buffers[thread_id];
    break;
  default:
    log_text(LOG_FATAL, "Invalid memory scope");
    return 1;
  }

  tls_buf_end = (size_t *) (void *) ((char *) tls_buf + memory_block_size);

  return rc;
  return 0;
}


sb_event_t memory_next_event(int thread_id)
{
  sb_event_t      req;

  (void) thread_id; /* unused */

  if (memory_total_size > 0 && !tls_total_ops--)
  {
    req.type = SB_REQ_TYPE_NULL;
    return req;
  }

  req.type = SB_REQ_TYPE_MEMORY;

  return req;
}

static void evt_before(const char *fn, int thread_id, struct timespec *start)
{
  per_exec_times_cnt[thread_id] = per_exec_times_cnt[thread_id] + 1;
  if (clock_gettime(CLOCK_MONOTONIC, start)) {
    log_text(LOG_FATAL, "%s %d %m\n", fn, thread_id);
    exit(1);
  }
}

static void evt_after(const char *fn, int thread_id, struct timespec *start)
{
  struct timespec stop, diff;

  if (clock_gettime(CLOCK_MONOTONIC, &stop)) {
    log_text(LOG_FATAL, "%s %d %m\n", fn, thread_id);
    exit(1);
  }
  diff_timespec(thread_id, start, &stop, &diff);
  if (per_exec_times[thread_id]) {
    if (diff.tv_nsec) {
      per_exec_times[thread_id] = per_exec_times[thread_id] + (diff.tv_nsec + (1000000000 * diff.tv_sec));
      if (diff.tv_nsec < per_exec_times_min[thread_id]) per_exec_times_min[thread_id] = diff.tv_nsec;
      if (diff.tv_nsec > per_exec_times_max[thread_id]) per_exec_times_max[thread_id] = diff.tv_nsec;
    } else {
      per_exec_times_miss[thread_id] = per_exec_times_miss[thread_id] + 1;
    }
  } else {
    per_exec_times[thread_id] = diff.tv_nsec;
    per_exec_times_min[thread_id] = diff.tv_nsec;
    per_exec_times_max[thread_id] = diff.tv_nsec;
  }
}

/*
  Use either 32- or 64-bit primitives depending on the native word
  size. ConcurrencyKit ensures the corresponding loads/stores are not optimized
  away by the compiler.
*/
#if SIZEOF_SIZE_T == 4
# define SIZE_T_LOAD(ptr) ck_pr_load_32((uint32_t *)(ptr))
# define SIZE_T_STORE(ptr,val) ck_pr_store_32((uint32_t *)(ptr),(uint32_t)(val))
#elif SIZEOF_SIZE_T == 8
# define SIZE_T_LOAD(ptr) ck_pr_load_64((uint64_t *)(ptr))
# define SIZE_T_STORE(ptr,val) ck_pr_store_64((uint64_t *)(ptr),(uint64_t)(val))
#else
# error Unsupported platform.
#endif

int event_rnd_none(sb_event_t *req, int thread_id)
{
  (void) req; /* unused */
  (void) thread_id; /* unused */
  struct timespec     start;
  evt_before(__func__, thread_id, &start);

  for (ssize_t i = 0; i < memory_block_size; i += SIZEOF_SIZE_T)
  {
    size_t offset = (volatile size_t) (sb_rand_uniform_double() *
                                       (memory_block_size / SIZEOF_SIZE_T));
    (void) offset; /* unused */
    /* nop */
  }

  evt_after(__func__, thread_id, &start);
  return 0;
}


int event_rnd_read(sb_event_t *req, int thread_id)
{
  (void) req; /* unused */
  (void) thread_id; /* unused */
  struct timespec     start;
  evt_before(__func__, thread_id, &start);

  for (ssize_t i = 0; i < memory_block_size; i += SIZEOF_SIZE_T)
  {
    size_t offset = (size_t) (sb_rand_uniform_double() *
                              (memory_block_size / SIZEOF_SIZE_T));
    size_t val = SIZE_T_LOAD(tls_buf + offset);
    (void) val; /* unused */
  }

  evt_after(__func__, thread_id, &start);
  return 0;
}


int event_rnd_write(sb_event_t *req, int thread_id)
{
  (void) req; /* unused */
  (void) thread_id; /* unused */
  struct timespec     start;
  evt_before(__func__, thread_id, &start);

  for (ssize_t i = 0; i < memory_block_size; i += SIZEOF_SIZE_T)
  {
    size_t offset = (size_t) (sb_rand_uniform_double() *
                              (memory_block_size / SIZEOF_SIZE_T));
    SIZE_T_STORE(tls_buf + offset, i);
  }

  evt_after(__func__, thread_id, &start);
  return 0;
}


int event_seq_none(sb_event_t *req, int thread_id)
{
  (void) req; /* unused */
  (void) thread_id; /* unused */
  struct timespec     start;
  evt_before(__func__, thread_id, &start);

  for (size_t *buf = tls_buf, *end = buf + memory_block_size / SIZEOF_SIZE_T;
       buf < end; buf++)
  {
    ck_pr_barrier();
    /* nop */
  }

  evt_after(__func__, thread_id, &start);
  return 0;
}


int event_seq_read(sb_event_t *req, int thread_id)
{
  (void) req; /* unused */
  (void) thread_id; /* unused */
  struct timespec     start;
  evt_before(__func__, thread_id, &start);

  for (size_t *buf = tls_buf, *end = buf + memory_block_size / SIZEOF_SIZE_T;
       buf < end; buf++)
  {
    size_t val = SIZE_T_LOAD(buf);
    (void) val; /* unused */
  }

  evt_after(__func__, thread_id, &start);
  return 0;
}


int event_seq_write(sb_event_t *req, int thread_id)
{
  (void) req; /* unused */
  (void) thread_id; /* unused */
  struct timespec     start;
  evt_before(__func__, thread_id, &start);

  for (size_t *buf = tls_buf, *end = buf + memory_block_size / SIZEOF_SIZE_T;
       buf < end; buf++)
  {
    SIZE_T_STORE(buf, end - buf);
  }

  evt_after(__func__, thread_id, &start);
  return 0;
}


void memory_print_mode(void)
{
  char *str;

  log_text(LOG_NOTICE, "Running memory speed test with the following options:");
  log_text(LOG_NOTICE, "  block size: %ldKiB",
           (long)(memory_block_size / 1024));
  log_text(LOG_NOTICE, "  total size: %ldMiB",
           (long)(memory_total_size / 1024 / 1024));

  switch (memory_oper) {
    case SB_MEM_OP_READ:
      str = "read";
      break;
    case SB_MEM_OP_WRITE:
      str = "write";
      break;
    case SB_MEM_OP_NONE:
      str = "none";
      break;
    default:
      str = "(unknown)";
      break;
  }
  log_text(LOG_NOTICE, "  operation: %s", str);

  switch (memory_scope) {
    case SB_MEM_SCOPE_GLOBAL:
      str = "global";
      break;
    case SB_MEM_SCOPE_LOCAL:
      str = "local";
      break;
    default:
      str = "(unknown)";
      break;
  }
  log_text(LOG_NOTICE, "  scope: %s", str);

  log_text(LOG_NOTICE, "");
}

/*
  Print intermediate test statistics.
*/

void memory_report_intermediate(sb_stat_t *stat)
{
  const double megabyte = 1024.0 * 1024.0;
  static char t[4096];
  size_t i, j = 0, k;
  unsigned long long tsc = __builtin_ia32_rdtsc();
  unsigned cnt = 0;

  for (i = 0; i < sb_globals.threads; i++) {
    cnt += per_exec_times_cnt[i];
    k = snprintf(t + j, sizeof(t) - j, "%6lx/%lx(%6lx %6lx %7lx) ", per_exec_times_cnt[i], per_exec_times_miss[i], per_exec_times_min[i], per_exec_times[i] / per_exec_times_cnt[i], per_exec_times_max[i]);
    per_exec_times[i] = 0;
    per_exec_times_cnt[i] = 0;
    per_exec_times_miss[i] = 0;
    per_exec_times_min[i] = 0;
    per_exec_times_max[i] = 0;
    if (k > sizeof(t) - j) exit(1);
    j += k;
  }
  log_timestamp(LOG_NOTICE, stat->time_total,
                "% 9.2f MiB/sec %7u: %16llx %s",
                stat->events * memory_block_size / megabyte / stat->time_interval,
                cnt, tsc, t);
}

/*
  Print cumulative test statistics.
*/

void memory_report_cumulative(sb_stat_t *stat)
{
  const double megabyte = 1024.0 * 1024.0;

  log_text(LOG_NOTICE, "Total operations: %" PRIu64 " (%8.2f per second)\n",
           stat->events, stat->events / stat->time_interval);

  if (memory_oper != SB_MEM_OP_NONE)
  {
    const double mb = stat->events * memory_block_size / megabyte;
    log_text(LOG_NOTICE, "%4.2f MiB transferred (%4.2f MiB/sec)\n",
             mb, mb / stat->time_interval);
  }

  sb_report_cumulative(stat);
}

#ifdef HAVE_LARGE_PAGES

/* Allocate memory from HugeTLB pool */

void * hugetlb_alloc(size_t size)
{
  int shmid;
  void *ptr;
  struct shmid_ds buf;

  /* Align block size to my_large_page_size */
  size = ((size - 1) & ~LARGE_PAGE_SIZE) + LARGE_PAGE_SIZE;

  shmid = shmget(IPC_PRIVATE, size, SHM_HUGETLB | SHM_R | SHM_W);
  if (shmid < 0)
  {
      log_errno(LOG_FATAL,
                "Failed to allocate %zd bytes from HugeTLB memory.", size);

      return NULL;
  }

  ptr = shmat(shmid, NULL, 0);
  if (ptr == (void *)-1)
  {
    log_errno(LOG_FATAL, "Failed to attach shared memory segment,");
    shmctl(shmid, IPC_RMID, &buf);

    return NULL;
  }

  /*
        Remove the shared memory segment so that it will be automatically freed
            after memory is detached or process exits
  */
  shmctl(shmid, IPC_RMID, &buf);

  return ptr;
}

#endif
