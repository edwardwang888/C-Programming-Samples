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
#include "SortedList.h"
#include <signal.h>

int num_threads, iterations, num_lists;
int debug;
char sync;
char test_name[100];
pthread_mutex_t *lock;
int *spin_lock;
int opt_yield;
SortedList_t *list;
#define KEYLEN 4
#define NUMCHARS 128
#define KEY 268435456 // equal to NUMCHARS^KEYLEN
char key_list[KEY][KEYLEN];
#define BILLION 1000000000
int *lock_ops;
struct timespec *thread_start, *thread_end;
long long *lock_time;
int get_time;


void error_and_exit2(const char* message, int error, const int line, int retval)
{
  fprintf(stderr, "ERROR: %s at line %d: %s\n", message, line, strerror(error));
  exit(retval);
}

void error_and_exit(const char* message, int error, const int line)
{
  error_and_exit2(message, error, line, 1);
}

void corrupted_list_exit(int line)
{
  fprintf(stderr, "Error: Corrupted list at line %d\n", line);
  exit(2);
}

void catch_sigsegv()
{
  fprintf(stderr, "Error: Segmentation fault\n");
  exit(2);
}

struct thread_data
{
  SortedListElement_t *elements;
  int thread_num;
  //  long long *lock_time;
};

void acquire_lock(int thread_i, int list_i)
{
  // Get start time
  if (get_time && clock_gettime(CLOCK_MONOTONIC, thread_start+thread_i) == -1)
    error_and_exit2("clock_gettime() failed", errno, __LINE__, 2);

  // Acquire lock
  if (sync == 'm') {
    pthread_mutex_lock(lock+list_i);
    if (get_time)
      lock_ops[thread_i]++;
  }
  else if (sync == 's') {
    while (__sync_lock_test_and_set(spin_lock+list_i, 1));
    if (get_time)
      lock_ops[thread_i]++;
  }
  // Get end time
  if (get_time) {
    if (clock_gettime(CLOCK_MONOTONIC, thread_end+thread_i) == -1)
      error_and_exit2("clock_gettime() failed", errno, __LINE__, 2);
    // Calculate elapsed time
    long long elapsed = BILLION * (thread_end[thread_i].tv_sec - thread_start[thread_i].tv_sec);
    elapsed += thread_end[thread_i].tv_nsec - thread_start[thread_i].tv_nsec;
    lock_time[thread_i] += elapsed;
  }
}

void release_lock(int list_i)
{
  if (sync == 'm') {
    pthread_mutex_unlock(lock+list_i);
  }
  else if (sync == 's') {
    __sync_lock_release(spin_lock+list_i);
  }
}

int hash(const char* key)
{
  // View key as a 4-bit, base 128 number (4-bit key, 128 possible characters per bit)
  // Convert this to decimal representation to get the hash
  int hash = 0;
  for (int i = 0; i < KEYLEN; i++) {
    int base = 1;
    for (int j = 1; j <= i; j++)
      base *= NUMCHARS; // base = NUMCHARS^i
    hash += (int)(key[i]) * base;
  }
  return hash % num_lists;
}

int getlen(int thread_i)
{
  // Get all locks
  for (int i = 0; i < num_lists; i++)
    acquire_lock(thread_i, i);
  int len = 0;
  for (int i = 0; i < num_lists; i++) {
    int temp = SortedList_length(list+i);
    if (temp < 0)
      corrupted_list_exit(__LINE__);
    len += temp;
  }
  // Release all locks
  for (int i = 0; i < num_lists; i++)
    release_lock(i);
  return len;
}

void *thread_run(void *arg)
{
  struct thread_data *thread_arg = (struct thread_data*)arg;
  SortedListElement_t *elements = thread_arg->elements;
  int thread_i = thread_arg->thread_num;
  //  long long *lock_time = thread_arg->lock_time;
  int lower = thread_i * iterations;
  //  printf("%d ", SortedList_length(&list));

  for (int i = lower; i < lower + iterations; i++) {
    int list_i = hash((elements+i)->key);
    acquire_lock(thread_i, list_i);
    //printf("Sending key: %d\n", (elements+i)->key);
    // Figure out which sublist
    SortedList_insert(list+list_i, elements+i);
    release_lock(list_i);
  }

  int len = getlen(thread_i);
  if (len < 0)
    corrupted_list_exit(__LINE__);
  /*
    if (debug)
    printf("Thread %d, length=%d\n", thread_i, len);
  */

  for (int i = lower; i < lower + iterations; i++) {
    const char *key = (elements+i)->key;
    int list_i = hash(key);
    acquire_lock(thread_i, list_i);
    SortedListElement_t *elt = SortedList_lookup(list+list_i, key);
    // Check for corruption
    if (elt == NULL || SortedList_delete(elt) == 1)
      corrupted_list_exit(__LINE__);
    release_lock(list_i);
  }

  //  printf("%d ", SortedList_length(&list));

  return 0;
}

void process_arguments(int argc, char* argv[])
{
  // Setup argument processing
  int longindex = -1;
  num_threads = 1;
  iterations = 1;
  num_lists = 1;
  opt_yield = 0;
  sync = '0'; // No sync
  debug = 0;
  get_time = 1;

  static struct option long_options[] = {
    {"threads", required_argument, 0, 0},
    {"iterations,", required_argument, 0, 0},
    {"yield", required_argument, 0, 0},
    {"sync", required_argument, 0, 0},
    {"lists", required_argument, 0, 0},
    {"debug", no_argument, 0, 0},
    {"no-time", no_argument, 0, 0},
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
      if (num_threads <= 0) {
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
      for (int i = 0; i < (int)strlen(optarg); i++) {
	switch (*(optarg+i)) {
	case 'i':
	  opt_yield |= INSERT_YIELD; break;
	case 'd':
	  opt_yield |= DELETE_YIELD; break;
	case 'l':
	  opt_yield |= LOOKUP_YIELD; break;
	}
      }
    }
    else if (longindex == 3) {
      sync = *optarg;
      if (sync != 'm' && sync != 's') {
	fprintf(stderr, "Invalid sync option: %s\n", optarg);
	exit(1);
      }
    }
    else if (longindex == 4) {
      num_lists = atoi(optarg);
      if (num_lists <= 0) {
	fprintf(stderr, "Invalid number of lists specified: %s\n", optarg);
	exit(1);
      }
    }
    else if (longindex == 5) {
      debug = 1;
    }
    else if (longindex == 6) {
      get_time = 0;
    }
  }
  // Create test name
  strcpy(test_name, "list-");
  char yield_type[5] = "";
  if (opt_yield & INSERT_YIELD)
    strcat(yield_type, "i");
  if (opt_yield & DELETE_YIELD)
    strcat(yield_type, "d");
  if (opt_yield & LOOKUP_YIELD)
    strcat(yield_type, "l");
  if (strcmp(yield_type, "") == 0)
    strcat(yield_type, "none");
  strcat(test_name, yield_type);
  strcat(test_name, "-");
  if (sync != '0')
    strncat(test_name, &sync, 1);
  else
    strcat(test_name, "none");

  // Artificial no-time
  //get_time = 0;
}

void init_elements(SortedListElement_t elements[], char key_list[num_threads*iterations][KEYLEN+1])
{
  for (int i = 0; i < num_threads*iterations; i++) {
    SortedListElement_t new_element;
    new_element.prev = NULL;
    new_element.next = NULL;
    for (int j = 0; j < KEYLEN; j++)
      key_list[i][j] = (char)(rand() % NUMCHARS);
    key_list[i][KEYLEN] = '\0';
    new_element.key = key_list[i];
    elements[i] = new_element;
    //    printf("key is: %s, length=%d\n", new_element.key, strlen(new_element.key));
  }
}


void write_csv(const char* output)
{
  int fd = open("lab2b_list.csv", O_WRONLY|O_CREAT|O_APPEND, S_IRUSR|S_IWUSR);
  if (fd == -1)
    error_and_exit("open() failed", errno, __LINE__);
  if (write(fd, output, strlen(output)) == -1)
    error_and_exit("write() failed", errno, __LINE__);
  if (close(fd) == -1)
    error_and_exit("close() failed", errno, __LINE__);
}

int main(int argc, char* argv[])
{
  // Setup signal handler
  signal(SIGSEGV, catch_sigsegv);
  
  // Process arguments
  process_arguments(argc, argv);
  if (debug)
    printf("threads=%d\niterations=%d\n", num_threads, iterations);

  // Initialize sublists
  list = malloc(sizeof(SortedList_t) * num_lists);
  for (int i = 0; i < num_lists; i++) {
    list[i].prev = NULL;
    list[i].next = NULL;
    list[i].key = NULL;
  }

  // Check for corruption; don't need mutex here
  for (int i = 0; i < num_lists; i++) {
    if (SortedList_length(list+i) != 0)
      corrupted_list_exit(__LINE__);
  }

  // Initialize elements
  SortedListElement_t elements[num_threads*iterations];
  char key_list[num_threads*iterations][KEYLEN+1];
  init_elements(elements, key_list);

  // Initialize mutex and locks
  lock = malloc(sizeof(pthread_mutex_t) * num_lists);
  spin_lock = malloc(sizeof(int) * num_lists);
  if (sync == 'm') {
    for (int i = 0; i < num_lists; i++)
      pthread_mutex_init(lock+i, NULL);
  }
  else if (sync == 's') {
    for (int i = 0; i < num_lists; i++)
      spin_lock[i] = 0;
  }

  // Initialize array to count lock time
  lock_time = malloc(sizeof(long) * num_threads);
  lock_ops = malloc(sizeof(int) * num_threads);
  thread_start = malloc(sizeof(struct timespec) * num_threads);
  thread_end = malloc(sizeof(struct timespec) * num_threads);
  for (int i = 0; i < num_threads; i++) {
    lock_time[i] = 0;
    lock_ops[i] = 0;
  }

  // Get start time
  struct timespec start;
  if (clock_gettime(CLOCK_MONOTONIC, &start) == -1)
    error_and_exit2("clock_gettime() failed", errno, __LINE__, 2);

  // Create and run threads
  struct thread_data arg[num_threads];
  pthread_t threads[num_threads];
  for (int i = 0; i < num_threads; i++) {
    arg[i].elements = elements;
    arg[i].thread_num = i;
    //arg[i].lock_time = lock_time;
    int c = pthread_create(&threads[i], NULL, thread_run, (void *)(arg+i));
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
  long long ops = num_threads * iterations * 3;
  long long average = runtime/ops;
  long long avg_lock_wait = 0;
  int total_lock_ops = 0;
  for (int i = 0; i < num_threads; i++)
    total_lock_ops += lock_ops[i];
  if (total_lock_ops != 0) {
    long long sum = 0;
    for (int i = 0; i < num_threads; i++)
      sum += lock_time[i];
    avg_lock_wait = sum/total_lock_ops;
  }

  // Check for corruption
  for (int i = 0; i < num_lists; i++) {
    if (SortedList_length(list+i) != 0)
      corrupted_list_exit(__LINE__);
  }

  // Write to stdout and CSV
  char output[100];
  sprintf(output, "%s,%d,%d,%d,%lld,%lld,%lld,%lld\n", test_name, num_threads, iterations, \
	  num_lists,ops, runtime, average, avg_lock_wait);
  printf(output);
  write_csv(output);

  // Free memory
  free(lock_time);
  free(lock);
  free(spin_lock);
  free(lock_ops);
  free(thread_start);
  free(thread_end);
}
