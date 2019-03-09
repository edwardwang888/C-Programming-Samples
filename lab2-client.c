#define _POSIX_SOURCE
#include <termios.h>
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <getopt.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>

#include <sys/types.h>
#include <signal.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include "zlib.h"

struct termios termios_save;
int server_fd, logfile_fd;
bool debug;

void reset_terminal()
{
  int c = tcsetattr(0, TCSANOW, &termios_save);
  int errsv = errno;
  if (c == -1) {
    fprintf(stderr, "ERROR: tcsetattr() failed at line %d: "
	    "could not reset file descriptor (0): %s\xD\xA", __LINE__, strerror(errsv));
    exit(1);
  }
}

void error_and_exit2(const char* message, const char* error, const int line, bool reset)
{
  fprintf(stderr, "ERROR: %s at line %d: %s\xD\xA", message, line, error);
  if (reset == true)
    reset_terminal();
  exit(1);
}

void error_and_exit(const char* message, const char* error, const int line)
{
  error_and_exit2(message, error, line, true);
}

void logfile_error(char* error, int line)
{
  error_and_exit("could not write to log: write() failed", error, line);
}

#define CHUNK 16384
int def(char *src, char *dest, const int SIZE, int level)
{
  if (debug == true)
    fprintf(stderr, "Starting deflate function (line %d)\xD\xA", __LINE__);
  
  int ret, flush;
  unsigned have;
  unsigned i = 0;
  z_stream strm;
  //unsigned char in[CHUNK];
  unsigned char out[CHUNK];

  if (debug == true)
    fprintf(stderr, "Finished declaring variables (line %d)\xD\xA", __LINE__);
  
  /* allocate deflate state */
  strm.zalloc = Z_NULL;
  strm.zfree = Z_NULL;
  strm.opaque = Z_NULL;
  if (debug == true)
    fprintf(stderr, "Before calling deflateInit() (line %d)\xD\xA", __LINE__);
  ret = deflateInit(&strm, level);
  if (debug == true)
    fprintf(stderr, "Finished calling deflateInit() (line %d)\xD\xA", __LINE__);
  if (ret != Z_OK)
    return ret;
  
  if (debug == true) {
    fprintf(stderr, "Finished allocating deflate state (line %d)\xD\xA", __LINE__);
    fprintf(stderr, "Byte size is: %d\xD\xA", SIZE);
  }

  strm.avail_in = SIZE;
  flush = Z_SYNC_FLUSH;
  strm.next_in = (Bytef*)src;
  do {
    strm.avail_out = CHUNK;
    strm.next_out = out;
    ret = deflate(&strm, flush);
    have = CHUNK - strm.avail_out;
    // Save to dest buffer
    strncpy(dest+i,(char*)out,have);
    i += have;
  } while (strm.avail_out == 0);
  // Insert null byte
  *(dest+i) = '\0';
  if (debug == true)
    fprintf(stderr, "Finished deflate() (line %d)\xD\xA", __LINE__);
  (void)deflateEnd(&strm);
  if (debug == true) {
    fprintf(stderr, "Finished compressing input (line %d)\xD\xA", __LINE__);
    fprintf(stderr, "Output buffer: %s\xD\xA", dest);
    fprintf(stderr, "Length of output buffer is: %d\xD\xA", (int)strlen(dest));
    fprintf(stderr, "Total bytes is: %d\xD\xA", i);
  }
  return i;
}

#define CHUNK 16384
int inf(char* src, char* dest, const int SIZE)
{
  int ret;
  unsigned have;
  unsigned i = 0;
  z_stream strm;
  unsigned char out[CHUNK];

  /* allocate inflate state */
  strm.zalloc = Z_NULL;
  strm.zfree = Z_NULL;
  strm.opaque = Z_NULL;
  strm.avail_in = 0;
  strm.next_in = Z_NULL;
  ret = inflateInit(&strm);
  if (ret != Z_OK)
    return ret;
  if (debug == true)
    fprintf(stderr, "Finished inflateInit() (line %d)\xD\xA", __LINE__);
  strm.avail_in = SIZE;
  strm.next_in = (Bytef*)src;
  do {
    strm.avail_out = CHUNK;
    strm.next_out = out;
    ret = inflate(&strm, Z_SYNC_FLUSH);

    if (debug == true)
      fprintf(stderr, "Value of ret is: %d\xD\xA", ret);
    /*
    switch(ret) {
    case Z_NEED_DICT:
      ret = Z_DATA_ERROR;
    case Z_DATA_ERROR:
    case Z_MEM_ERROR:
      (void)inflateEnd(&strm);
      return ret;
    }
    */
    if (debug == true)
      fprintf(stderr, "Value of ret is: %d\xD\xA", ret);

    have = CHUNK - strm.avail_out;
    // Write out to dest
    strncpy(dest+i, (char*)out, have);
    i += have;
  } while (strm.avail_out == 0);

  // Set null byte
  *(dest+i) = '\0';
  
  if (debug == true) {
    fprintf(stderr, "Finished inflate() (line %d)\xD\xA", __LINE__);
    fprintf(stderr, "Input buffer: %s\xD\xA", src);
    fprintf(stderr, "Output buffer: %s\xD\xA", dest);
    fprintf(stderr, "Length of output buffer: %d\xD\xA", (int)strlen(dest));
    fprintf(stderr, "Value of i is: %d\xD\xA", i);
  }

  (void)inflateEnd(&strm);
  if (debug == true)
    fprintf(stderr, "Finished inflateEnd() (line %d)\xD\xA", __LINE__);
  if (debug == true)
    fprintf(stderr, "Returning from inf() now (line %d)\xD\xA", __LINE__);
  return Z_OK;
}

/* report a zlib or i/o error */
void zerr(int ret)
{
  fputs("zpipe: ", stderr);
  switch (ret) {
  case Z_ERRNO:
    if (ferror(stdin))
      fputs("error reading stdin\n", stderr);
    if (ferror(stdout))
      fputs("error writing stdout\n", stderr);
    break;
  case Z_STREAM_ERROR:
    fputs("invalid compression level\n", stderr);
    break;
  case Z_DATA_ERROR:
    fputs("invalid or incomplete deflate data\n", stderr);
    break;
  case Z_MEM_ERROR:
    fputs("out of memory\n", stderr);
    break;
  case Z_VERSION_ERROR:
    fputs("zlib version mismatch!\n", stderr);
  }
}

void compress_input_and_write(char* compress_in, char* compress_out, const int bytes_read, bool log)
{
  if (debug == true)
    fprintf(stderr, "Compressing input (line %d)\xD\xA", __LINE__);
  int compress_size = def(compress_in, compress_out, bytes_read, Z_DEFAULT_COMPRESSION);
  compress_size = 4; 
  //compress_size = (int)strlen(compress_out);

  if (debug == true) {
    fprintf(stderr, "Uncompressed input: %s\xD\xA", compress_in);
    fprintf(stderr, "Compressed buffer: %s\xD\xA", compress_out);
    fprintf(stderr, "Length of compressed buffer: %d\xD\xA", compress_size);
    fprintf(stderr, "Contents of buffer: ");
    write(2, compress_out, 100);
    fprintf(stderr, "\xD\xA");
  }
  // Write compressed input out to server and log
  if (write(server_fd, compress_out, compress_size) == -1)
    error_and_exit("could not write to server: write() failed", strerror(errno), __LINE__);
  if (log == true && write(logfile_fd, compress_out, compress_size) == -1)
    logfile_error(strerror(errno), __LINE__);
}

void process_input(bool sigpipe, bool log, bool compress)
{
  struct pollfd fds[2];
    
  // Set keyboard poll
  fds[0].fd = 0;
  fds[0].events = POLLIN;
    
  // Set server poll
  fds[1].fd = server_fd;
  fds[1].events = POLLIN;

  // Read input
  const int SIZE = 256;
  char buf[256];

  // Set up buffers for compression
  char compress_in[SIZE], compress_out[CHUNK];

  while(1) {
    int c = poll(fds,2,0);
    int errsv = errno;
    if (c < 0) {
      error_and_exit("poll() failed", strerror(errsv), __LINE__);
    }
    else if (sigpipe == true && c == 0) {
      if (debug == true)
	fprintf(stderr, "sigpipe option on (line %d)\xD\xA", __LINE__);
      goto end;
    }
    else if (c > 0) {
      // Check which poll succeeded
      int bytes_read;
      if (fds[0].revents != 0)
	bytes_read = read(0, buf, SIZE);
      else if (fds[1].revents != 0)
	bytes_read = read(server_fd, buf, SIZE);
      else { // This should never occur
	fprintf(stderr, "Error in poll: no ready file descriptor found.\n");
      }
      int errsv = errno;
      if (bytes_read < 0) {
	if (fds[0].revents != 0)
	  error_and_exit("could not read from stdin: read() failed", strerror(errsv), __LINE__);
	else
	  error_and_exit("could not read from socket: "
			 "read() failed", strerror(errsv), __LINE__);
      }
	
      // Check for EOF; this should never happen for the keyboard
      if (bytes_read == 0) {
	if (fds[0].revents != 0 && debug == true) // This should never occur
	  fprintf(stderr, "Error!! read(0) indicates return value of zero.\n");
	// fprintf(stderr, "Client is exiting\xD\xA");
	goto end;
      }

      if (log == true) {
	char msg[500];
	if (fds[0].revents != 0)
	  sprintf(msg, "SENT %d bytes: ", bytes_read);
	else
	  sprintf(msg, "RECEIVED %d bytes: ", bytes_read);
	if (write(logfile_fd, msg, strlen(msg)) == -1)
	  logfile_error(strerror(errno), __LINE__-1);
	// fprintf(stderr, msg);
      }

      // Decompress received input if necessary
      if (fds[1].revents != 0 &&  compress == true) {
	// Log if necessary
	if (log == true && write(logfile_fd, buf, bytes_read) == -1) {
	  logfile_error(strerror(errno), __LINE__);
	}
	int ret = inf(buf, compress_out, bytes_read);
	if (ret != Z_OK) {
	  zerr(ret);
	  exit(1);
	}
	strcpy(buf, compress_out);
	bytes_read = strlen(buf);
      }
      if (debug == true) {
	fprintf(stderr, "buf is now: %s\xD\xA", buf);
	fprintf(stderr, "bytes_read is: %d\xD\xA", bytes_read);
	fprintf(stderr, "length of buf is: %d\xD\xA", (int)strlen(buf));
      }

      // Write buffer
      for (int i = 0; i < bytes_read; i++) {
	switch(*(buf+i)) {
	case 13:
	  // Receive <CR> from keyboard, map to <CR><LF> for stdout, <LF> for bash
	  if (fds[0].revents != 0) {
	    if (write(1, "\xD\xA", 2) == -1) {
	      error_and_exit("could not write to stdout: write(1) failed", \
			     strerror(errno), __LINE__);
	    }
	    if (compress == false && sigpipe == false && write(server_fd, "\xA", 1) == -1) {
	      int errsv = errno;
	      char msg[500];
	      sprintf(msg, "could not write to server: write(%d) failed", server_fd);
	      error_and_exit(msg, strerror(errsv), __LINE__);
	    }
	    else if (compress == true && sigpipe == false) {
	      compress_in[i] = '\xA';
	    }
	    if (compress == false && log == true && sigpipe == false) {
	      if (write(logfile_fd, "\xA", 1) == -1)
		logfile_error(strerror(errno), __LINE__);
	    }
	    
	    if (log == true && debug == true)
	      printf("Logging at line %d\xD\xA", __LINE__);
	  }
	  // Receive <CR> from shell, write to stdout unmodified
	  else if (fds[1].revents != 0) {
	    if (write(1, buf+i, 1) == -1) {
	      error_and_exit("could not write to stdout: write(1) failed", \
			     strerror(errno), __LINE__);
	    }
	    
	    if (log == true && compress == false && write(logfile_fd, buf+i, 1) == -1) {
	      logfile_error(strerror(errno), __LINE__);
	    }
	    
	    if (log == true && debug == true)
	      printf("Writing to log file at line %d!\xD\xA", __LINE__);
	  }
	  break;

	case 10:
	  // Receive <LF> from keyboard, map to <CR><LF> for stdout, <LF> for bash
	  if (fds[0].revents != 0) {
	    if (write(1, "\xD\xA", 2) == -1) {
	      error_and_exit("could not write to stdout: write(1) failed", \
			     strerror(errno), __LINE__);
	    }
	    if (compress == false && sigpipe == false && write(server_fd, buf+i, 1) == -1) {
	      int errsv = errno;
	      char msg[500];
	      sprintf(msg, "could not write to server: write(%d) failed", server_fd);
	      error_and_exit(msg, strerror(errsv), __LINE__);
	    }
	    else if (compress == true && sigpipe == false) {
	      compress_in[i] = *(buf+i);
	    }
	    if (log == true && sigpipe == false && write(logfile_fd, buf+i, 1) == -1) {
	      logfile_error(strerror(errno), __LINE__);
	    }
	    if (log == true && debug == true)
	      fprintf(stderr, "Logging at line %d\xD\xA", __LINE__);
	  }
	  // Receive <LF> from shell, map to <CR><LF> for stdout
	  else {
	    if (write(1, "\xD\xA", 2) == -1) {
	      error_and_exit("could not write to stdout: write(1) failed", \
			     strerror(errno), __LINE__);
	    }
	    if (log == true && compress == false && write(logfile_fd, "\xD\xA", 2) == -1) {
	      logfile_error(strerror(errno), __LINE__);
	    }
	  }
	  break;

	default:
	  if (write(1, buf+i, 1) == -1)
	    error_and_exit("could not write to stdout: write(1) failed", \
			   strerror(errno), __LINE__);
	  // Received input from keyboard
	  if (fds[0].revents != 0) {
	    if (debug == true)
	      fprintf(stderr, "Received input from keyboard (line %d)\xD\xA", __LINE__);
	    if (compress == false && sigpipe == false && write(server_fd, buf+i, 1) == -1) {
	      int errsv = errno;
	      char msg[500];
	      sprintf(msg, "could not write to server: write(%d) failed", server_fd);
	      error_and_exit(msg, strerror(errsv), __LINE__);
	    }
	    else if (compress == true && sigpipe == false) {
	      if (debug == true)
		fprintf(stderr, "Compressing keyboard to server (line %d)\xD\xA", __LINE__);
	      *(compress_in+i) = *(buf+i);
	    }
	    if (log == true && sigpipe == false) {
	      if (compress == false && write(logfile_fd, buf+i, 1) == -1) {
		logfile_error(strerror(errno), __LINE__);
	      }
	    }
	    if (log == true && log == false)
	      fprintf(stderr, "Logging at line %d\xD\xA", __LINE__);
	  }
	  // Received input from socket
	  else {
	    if (log == true && compress == false && write(logfile_fd, buf+i, 1) == -1) {
	      logfile_error(strerror(errno), __LINE__);
	    }
	    if (log == true && debug == true) {
	      fprintf(stderr, "Logging at line %d\xD\xA", __LINE__);
	    }
	  }
	  break;
	}    
      }
      // Compress input if needed
      if (compress == true) {
	compress_input_and_write(compress_in, compress_out, bytes_read, log);
      }
      if (log == true && write(logfile_fd, "\n", 1) == -1) {
	logfile_error(strerror(errno), __LINE__-1);
      }
    }
    // Reset revents
    fds[0].revents = 0;
    fds[1].revents = 0;
  }
 end:;
}

int main(int argc, char* argv[])
{
  // Save terminal attributes
  if (tcgetattr(0, &termios_save) == -1)
    fprintf(stderr, "ERROR: tcgetattr() failed at line %d: %s\n", __LINE__, strerror(errno));
  
  // Setup argument processing
  int port = 0;
  int longindex;
  bool log = false, compress = false;
  debug = false;
  char* logfile;
  static struct option long_options[] = {
    {"port", required_argument, 0, 0},
    {"log", required_argument, 0, 0},
    {"compress", no_argument, 0, 0},
    {"debug", no_argument, 0, 0},
    {0, 0, 0, 0}
  };
  
  while(1) {
    int c = getopt_long(argc, argv, ":", long_options, &longindex);
    if (c == -1)
      break;
    else if (c == '?') {
      fprintf(stderr, "Unrecognized option: %s\xD\xA", argv[optind-1]);
      // reset_terminal();
      exit(1);
    }
    else if (c == ':') {
      fprintf(stderr, "Missing required argument: %s\xD\xA", argv[optind-1]);
      // reset_terminal();
      exit(1);
    }
    if (longindex == 0) {
      port = atoi(optarg);
      if (port == 0) {
	fprintf(stderr, "Invalid port number: %s\n", optarg);
	exit(1);
      }
    }
    else if (longindex == 1) {
      log = true;
      logfile = optarg;
      // printf("logfile: %s\n", logfile);
    }
    else if (longindex == 2){
      compress = true;
    }
    else if (longindex == 3)
      debug = true;
  }
  /*
  if (port == 0) {
    fprintf(stderr, "Port number not specified: --port\n");
    exit(1);
  }
  */
  
  // printf("optarg: %s\n", optarg);
  // printf("logfile: %s\n", logfile);
  
  // printf("Finished argument processing\n");
  
  // Open logfile
  if (log == true) {
    logfile_fd = open(logfile, O_WRONLY|O_CREAT|O_TRUNC, S_IRUSR|S_IWUSR);
    if (logfile_fd == -1) {
      int errsv = errno;
      char msg[500];
      sprintf(msg, "could not open %s: open() failed", logfile);
      // Exit but WITHOUT resetting terminal
      error_and_exit2(msg, strerror(errsv), __LINE__, false);
    }
  }
  
  // Create socket
  {
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int errsv = errno;
    if (server_fd == -1)
      error_and_exit("socket() failed", strerror(errsv), __LINE__);
  }

  // Get host address
  struct hostent *host_info;
  {
    host_info = gethostbyname("localhost"); // Optional --host option
    int errsv = errno;
    if (host_info == NULL)
      error_and_exit("gethostbyname() failed", strerror(errsv), __LINE__);
  }
  
  struct sockaddr_in serv_addr;
  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  memcpy(&serv_addr.sin_addr.s_addr, host_info->h_addr_list[0], host_info->h_length);
  serv_addr.sin_port = htons(port); // htons(portno)

  // Connect to server
  if (connect(server_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1)
    error_and_exit("connect() failed", strerror(errno), __LINE__);
  
  if (debug == true)
    printf("Connected to server! (line %d)\n", __LINE__);

  // Set new attributes
  struct termios termios_new = termios_save;
  termios_new.c_iflag = ISTRIP;
  termios_new.c_oflag = 0;
  termios_new.c_lflag = 0;
  int c = tcsetattr(0, TCSANOW, &termios_new);
  int errsv = errno;
  if (c == -1) {
    error_and_exit("could not set stdin: tcsetattr() failed", strerror(errsv), __LINE__);
  }

  int exit_value = 0;
  const bool sigpipe = false;
  process_input(sigpipe, log, compress);

  if (debug == true)
    printf("Successfully processed input\xD\xA");
  
  // Close logfile
  if (log == true && close(logfile_fd) == -1) {
    int errsv = errno;
    char msg[500];
    sprintf(msg, "could not close %s: close() failed", logfile);
    error_and_exit(msg, strerror(errsv), __LINE__);
  }

  // Reset terminal attributes
  reset_terminal();
  exit(exit_value);
}
