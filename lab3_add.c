#define _POSIX_C_SOURCE 199309L
#include <getopt.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

int num_threads, iterations;
int opt_yield;
int debug;
char sync;
char test_name[100];
pthread_mutex_t lock;
int spin_lock;

void error_and_exit2(const char* message, int error, const int line, int retval)
{
  fprintf(stderr, "ERROR: %s at line %d: %s\n", message, line, strerror(error));
  exit(retval);
}

void error_and_exit(const char* message, int error, const int line)
{
  error_and_exit2(message, error, line, 1);
}

void add(long long *pointer, long long value)
{
  if (sync == 'm') {
    pthread_mutex_lock(&lock);
  }
  else if (sync == 's') {
    while (__sync_lock_test_and_set(&spin_lock, 1));
  }

  long long old, sum;

  if (sync != 'c')
    sum = *pointer + value;
  else {
    do {
      old = *pointer;
      if (opt_yield)
	sched_yield();
    } while (__sync_val_compare_and_swap(pointer, old, old + value) != old);
  }

  if (sync != 'c' && opt_yield)
    sched_yield();
  
  if (sync != 'c')
    *pointer = sum;

  if (sync == 'm') {
    pthread_mutex_unlock(&lock);
  }
  else if (sync == 's') {
    __sync_lock_release(&spin_lock);
  }
}

void *thread_start(void *counter)
{
  for (int i = 0; i < iterations; i++)
    add((long long *)counter, 1);
  for (int i = 0; i < iterations; i++)
    add((long long *)counter, -1);
  return 0;
}

void process_arguments(int argc, char* argv[])
{
  // Setup argument processing
  int longindex = -1;
  num_threads = 1;
  iterations = 1;
  opt_yield = 0;
  sync = '0'; // No sync
  debug = 0;
  strcpy(test_name, "add-");
  static struct option long_options[] = {
    {"threads", required_argument, 0, 0},
    {"iterations,", required_argument, 0, 0},
    {"yield", no_argument, 0, 0},
    {"sync", required_argument, 0, 0},
    {"debug", no_argument, 0, 0},
    {0, 0, 0, 0}
  };

  while(1) {
    int c = getopt_long(argc, argv, ":", long_options, &longindex);
    if (c == -1)
      break;
    else if (c == '?') {
      fprintf(stderr, "Unrecognized option: %s\xD\xA", argv[optind-1]);
      exit(1);
    }
    else if (c == ':') {
      fprintf(stderr, "Missing required argument: %s\xD\xA", argv[optind-1]);
      exit(1);
    }
    
    if (longindex == 0) {
      num_threads = atoi(optarg);
      if (num_threads == 0) {
	fprintf(stderr, "Invalid number of threads specified: %s\n", optarg);
	exit(1);
      }
    }
    else if (longindex == 1) {
      iterations = atoi(optarg);
      if (iterations == 0) {
	fprintf(stderr, "Invalid number of iterations specified: %s\n", optarg);
	exit(1);
      }
    }
    else if (longindex == 2) {
      opt_yield = 1;
      strcpy(test_name, "add-yield-");
    }
    else if (longindex == 3) {
      sync = *optarg;
      if (sync != 'm' && sync != 's' && sync != 'c') {
	fprintf(stderr, "Invalid sync option: %s\n", optarg);
	exit(1);
      }
    }
    else if (longindex == 4) {
      debug = 1;
    }
  }
  // Add sync type to name
  if (sync != '0')
    strncat(test_name, &sync, 1);
  else
    strcat(test_name, "none");
}


void write_csv(const char* output)
{
  int fd = open("lab2_add.csv", O_WRONLY|O_CREAT|O_APPEND, S_IRUSR|S_IWUSR);
  if (fd == -1)
    error_and_exit("open() failed", errno, __LINE__);
  if (write(fd, output, strlen(output)) == -1)
    error_and_exit("write() failed", errno, __LINE__);
  if (close(fd) == -1)
    error_and_exit("close() failed", errno, __LINE__);
}


int main(int argc, char* argv[])
{
  // Process arguments
  process_arguments(argc, argv);
  if (debug)
    printf("threads=%d\niterations=%d\n", num_threads, iterations);

  // Initialize mutex and locks
  if (sync == 'm') {
    pthread_mutex_init(&lock, NULL);
  }
  else if (sync == 's') {
    spin_lock = 0;
  }

  // Get start time
  struct timespec start;
  if (clock_gettime(CLOCK_MONOTONIC, &start) == -1)
    error_and_exit2("clock_gettime() failed", errno, __LINE__, 2);

  // Create and run threads
  long long counter = 0;
  if (debug)
    printf("counter before: %lld\n", counter);

  pthread_t threads[num_threads];
  for (int i = 0; i < num_threads; i++) {
    int c = pthread_create(&threads[i], NULL, thread_start, (void *)&counter);
    if (c != 0)
      error_and_exit2("p_thread_create() failed", c, __LINE__, 2);
  }

  for (int i = 0; i < num_threads; i++) {
    int c = pthread_join(threads[i], NULL);
    if (c != 0)
      error_and_exit2("p_thread_join() failed", c, __LINE__, 2);
  }

  // Get end time
  struct timespec end;
  if (clock_gettime(CLOCK_MONOTONIC, &end) == -1)
    error_and_exit2("clock_gettime() failed", errno, __LINE__, 2);
  long long runtime = 1000000000 * (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec);
  int ops = num_threads * iterations * 2;
  long long average = runtime/ops;


  if (debug)
    printf("counter after: %lld\n", counter);

  // Write to stdout and CSV
  char output[100];
  sprintf(output, "%s,%d,%d,%d,%lld,%lld,%lld\n", test_name, num_threads, iterations, ops, runtime, \
	  average, counter);
  printf(output);
  write_csv(output);
}
