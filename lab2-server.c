#define _POSIX_SOURCE
#include <termios.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <poll.h>
#include <getopt.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>

#include <sys/types.h>
#include <signal.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include "zlib.h"

bool debug, _compress;
int child_pid, client_sockfd;
int pipefd_to_bash[2], pipefd_to_term[2];

void error_and_exit(const char* message, const char* error, const int line)
{
  fprintf(stderr, "ERROR: %s at line %d: %s\n", message, line, error);
  exit(1);
}

void error_and_exit_child(const char* message, const char* error, int parentfd, int line)
{
  // We don't reset terminal here since fd 0 has likely been replaced already
  write(parentfd, "\x4", 1);
  fprintf(stderr, "ERROR: %s at line %d: %s\xD\xA", message, line, error);
  exit(1);
}

int print_exit_status()
{
  int wstatus = 0;
  if (debug == true)
    fprintf(stderr, "Waiting for child...\xD\xA");
  if (wait(&wstatus) == -1)
    error_and_exit("wait() failed", strerror(errno), __LINE__);
  if (debug == true)
    fprintf(stderr, "Finished waiting for child\xD\xA");
  const int SHELL_SIGNAL = wstatus & 0x007f;
  const int SHELL_STATUS = (wstatus & 0xff00)>>8;
  fprintf(stderr, "SHELL EXIT SIGNAL=%d STATUS=%d\xD\xA", SHELL_SIGNAL, SHELL_STATUS);
  return SHELL_STATUS;
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
  
    switch(ret) {
    case Z_NEED_DICT:
      ret = Z_DATA_ERROR;
    case Z_DATA_ERROR:
    case Z_MEM_ERROR:
      (void)inflateEnd(&strm);
      return ret;
    }

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
  return ret == Z_OK ? Z_OK : Z_DATA_ERROR;
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

void compress_input_and_write(char* compress_in, char* compress_out, const int bytes_read)
{
  if (debug == true)
    fprintf(stderr, "Compressing input (line %d)\xD\xA", __LINE__);
  int compress_size = def(compress_in, compress_out, bytes_read, Z_DEFAULT_COMPRESSION);
  //compress_size = 4; //(int)strlen(compress_out);

  if (debug == true) {
    fprintf(stderr, "Uncompressed input: %s\xD\xA", compress_in);
    fprintf(stderr, "Compressed buffer: %s\xD\xA", compress_out);
    fprintf(stderr, "Length of compressed buffer: %d\xD\xA", compress_size);
    fprintf(stderr, "Contents of buffer: ");
    write(2, compress_out, 100);
    fprintf(stderr, "\xD\xA");
  }
  // Write compressed input out to client
  if (write(client_sockfd, compress_out, compress_size) == -1)
    error_and_exit("could not write to server: write() failed", strerror(errno), __LINE__);

  if (debug == true)
    fprintf(stderr, "Finished compress_input_and_write() (line %d)\xD\xA", __LINE__);
}

void process_input(bool sigpipe)
{
  struct pollfd fds[2];
    
  // Set socket poll
  fds[0].fd = client_sockfd;
  fds[0].events = POLLIN;
    
  // Set bash poll
  fds[1].fd = pipefd_to_term[0];
  fds[1].events = POLLIN;

  // Read input
  const int SIZE = 256;
  char buf[256];
  
  // Set up buffers for compression
  char compress_in[SIZE], compress_out[SIZE];

  while(1) {
    int c = poll(fds,2,0);
    int errsv = errno;
    if (c < 0) {
      error_and_exit("poll() failed", strerror(errsv), __LINE__);
    }
    else if (sigpipe == true && c == 0) {
      if (debug == true)
	fprintf(stderr, "sigpipe and exit (line %d)\xD\xA", __LINE__);
      goto end;
    }
    else if (c > 0) {
      // Check which poll succeeded
      int bytes_read;
      if ((fds[0].revents & POLLIN) != 0) {
	bytes_read = read(client_sockfd, buf, SIZE);
	if (debug == true) {
	  fprintf(stderr, "Received input from keyboard! (line %d)\xD\xA", __LINE__);
	  fprintf(stderr, "%d %d\xD\xA", fds[0].revents, fds[1].revents);
	}
      }
      else if ((fds[1].revents & POLLIN) != 0) {
	//fds[0].revents = 0;
	bytes_read = read(pipefd_to_term[0], buf, SIZE);
	if (debug == true)
	  fprintf(stderr, "Received input from shell! (line %d)\xD\xA", __LINE__);
      }
      else {
	if (debug == true)
	  fprintf(stderr, "Error in poll: no ready file descriptor found.\n");
	goto end;
      }
      int errsv = errno;
      if (bytes_read < 0) {
	if (fds[0].revents != 0)
	  error_and_exit("could not read from socket: read() failed", strerror(errsv), __LINE__);
	else
	  error_and_exit("could not read from shell: "
			 "read() failed", strerror(errsv), __LINE__);
      }
	
      // Check for EOF; this should never happen for the socket
      if (bytes_read == 0) {
	if (debug == true)
	  fprintf(stderr, "No bytes read! (line %d)\xD\xA", __LINE__);
	
	goto end;
      }

      // Decompress received input if necessary
      if (fds[0].revents != 0 &&  fds[1].revents == 0 && _compress == true) {
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
      int count = 0;
      for (int i = 0; i < bytes_read; i++) {
	switch(*(buf+i)) {
	case 3:
	  if (sigpipe == false && kill(child_pid, SIGINT) == -1) {
	    if (debug == true)
	      fprintf(stderr, "Killing shell...\xD\xA");
	    int errsv = errno;
	    char msg[200];
	    sprintf(msg, "sending SIGINT to process %d: kill() failed", child_pid);
	    error_and_exit(msg, strerror(errsv), __LINE__);
	  }
	  break;
	case 4:
	  // Close pipe to shell
	  if (debug == true)
	    fprintf(stderr, "Closing pipe to shell (line %d)\xD\xA", __LINE__);
	  if (fds[0].revents != 0 && close(pipefd_to_bash[1]) == -1) {
	    int errsv = errno;
	    char msg[200];
	    sprintf(msg, "could not close pipefd_to_bash[1]: close(%d) failed", pipefd_to_bash[1]);
	    error_and_exit(msg, strerror(errsv), __LINE__);
	  }
	  break;

	case 13:
	  // Receive <CR> from stdin, map to <CR><LF> for stdout, <LF> for bash
	  // EDIT: Receive <CR> from socket, map to <LF> for bash
	  if (fds[0].revents != 0 && fds[1].revents == 0) {
	    /*
	    if (write(1, "\xD\xA", 2) == -1) {
	      error_and_exit("could not write to stdout: write(1) failed", \
			     strerror(errno), __LINE__);
	    }
	    */
	    if (sigpipe == false && write(pipefd_to_bash[1], "\xA", 1) == -1) {
	      int errsv = errno;
	      char msg[200];
	      sprintf(msg, "could not write to pipefd_to_bash[1]: "
		      "write(%d) failed", pipefd_to_bash[1]);
	      error_and_exit(msg, strerror(errsv), __LINE__);
	    }
	  }
	  // Receive <CR> from shell, write to socket unmodified
	  else if (fds[1].revents != 0) {
	    if (_compress == false && write(client_sockfd, buf+i, 1) == -1)
	      error_and_exit("could not write to socket: write() failed", \
			     strerror(errno), __LINE__);
	    else if (_compress == true) {
	      compress_in[count] = *(buf+i);
	      count++;
	    }
	  }
	  break;

	case 10:
	  // Receive <LF> from keyboard, map to <CR><LF> for stdout, <LF> for bash
	  // EDIT: Receive <LF> from socket, write to bash unmodified
	  if (fds[0].revents != 0 && fds[1].revents == 0) {
	    /*
	    if (write(1, "\xD\xA", 2) == -1) {
	      error_and_exit("could not write to stdout: write(1) failed", \
			     strerror(errno), __LINE__);
	    }
	    */
	    if (sigpipe == false && write(pipefd_to_bash[1], buf+i, 1) == -1) {
	      int errsv = errno;
	      char msg[200];
	      sprintf(msg, "could not write to pipefd_to_bash[1]: "
		      "write(%d) failed", pipefd_to_bash[1]);
	      error_and_exit(msg, strerror(errsv), __LINE__);
	    }
	  }
	  // Receive <LF> from shell, map to <CR><LF> for socket
	  else if (fds[1].revents != 0) {
	    if (_compress == false && write(client_sockfd, "\xD\xA", 2) == -1)
	      error_and_exit("could not write to socket: write() failed", \
			     strerror(errno), __LINE__);
	    else if (_compress == true) {
	      compress_in[count] = '\xD';
	      compress_in[count+1] = '\xA';
	      count += 2;
	    }
	  }
	  break;

	default:
	  /*
	  if (write(1, buf+i, 1) == -1)
	    error_and_exit("could not write to stdout: write(1) failed", \
			   strerror(errno), __LINE__);
	  */
	  // Process input from socket
	  if (fds[0].revents != 0 && fds[1].revents == 0) {
	    if (debug == true)
	      fprintf(stderr, "Write to shell: %c\xD\xA", *(buf+i));
	    if (sigpipe == false && write(pipefd_to_bash[1], buf+i, 1) == -1) {
	      int errsv = errno;
	      char msg[200];
	      sprintf(msg, "could not write to pipefd_to_bash[1]: "
		      "write(%d) failed", pipefd_to_bash[1]);
	      error_and_exit(msg, strerror(errsv), __LINE__);
	    }
	  }
	  // Process input from shell
	  else if (fds[1].revents != 0) {
	    if (_compress == false && write(client_sockfd, buf+i, 1) == -1) {
	      error_and_exit("could not write to socket: write() failed", strerror(errno), __LINE__);
	    }
	    else if (_compress == true) {
	      compress_in[count] = *(buf+i);
	      count++;
	    }
	  }
	  break;
	}
      }
      // Compress input if needed
      if (_compress == true && fds[1].revents != 0) {
	compress_input_and_write(compress_in, compress_out, count);
      }
    }
    // Reset revents
    fds[0].revents = 0;
    fds[1].revents = 0;
    /*    
    if (debug == true)
      fprintf(stderr, "Finished resetting revents (line %d)\xD\xA", __LINE__);
    */
  }
 end:;
  if (debug == true)
    fprintf(stderr, "Reached end of process_input() (line %d)\xD\xA", __LINE__);
}

void catch_sigpipe()
{
  if (debug == true)
    fprintf(stderr, "Caught SIGPIPE\xD\xA");
  process_input(true);
  if (debug == true)
    fprintf(stderr, "Finished processing input\xD\xA");
  const int SHELL_STATUS = print_exit_status();
  exit(SHELL_STATUS);
}

void replace_child_fds()
{
  // Replace stdin with pipe from terminal process
  if (close(0) == -1) {
    write(pipefd_to_term[1], "\x4", 1); // This is redundant
    error_and_exit("could not close stdin: close(0) failed", strerror(errno), __LINE__);
  }
  if (dup2(pipefd_to_bash[0], 0) == -1) {
    write(pipefd_to_term[1], "\x4", 1); // This is redundant
    fprintf(stderr, "ERROR: could not duplicate pipefd_to_bash[0]: dup2(%d,%d) "
	    "failed at line %d: %s\xD\xA", pipefd_to_bash[0], 0, __LINE__, strerror(errno));
    exit(1);
  }
  if (close(pipefd_to_bash[0]) == -1) {
    write(pipefd_to_term[1], "\x4", 1); // This is redundant
    fprintf(stderr, "ERROR: could not close pipefd_to_bash[0]: close(%d) "
	    "failed at line %d: %s\xD\xA", pipefd_to_bash[0], __LINE__, strerror(errno));      
    exit(1);
  }
  if (close(pipefd_to_bash[1]) == -1) {
    // Note: switching fprintf and write causes terminal not to output properly?
    fprintf(stderr, "ERROR: could not close pipefd_to_bash[1]: close(%d) "
	    "failed at line %d: %s\xD\xA", pipefd_to_bash[1], __LINE__, strerror(errno));
    write(pipefd_to_term[1], "\x4", 1); // This is redundant
    exit(1);
  }

  // Replace stdout and stderror with pipe to terminal process
  if (close(1) == -1) {
    error_and_exit_child("could not close stdout: "
			 "close(1) failed", strerror(errno), pipefd_to_term[1], __LINE__);
  }
    
  if (dup2(pipefd_to_term[1], 1) == -1) {
    int errsv = errno;
    char msg[200];
    sprintf(msg, "could not duplicate pipefd_to_term[1]: dup2(%d,1) failed", pipefd_to_term[1]);
    error_and_exit_child(msg, strerror(errsv), pipefd_to_term[1], __LINE__);
  }

  // Save stderr before closing
  int d = dup(2);
  int errsv = errno;
  if (d == -1) {
    error_and_exit_child("could not duplicate stderr: "
			 "dup(2) failed", strerror(errsv), pipefd_to_term[1], __LINE__);
  }
    
  if (close(2) == -1) {
    error_and_exit_child("could not close stderr: "
			 "close(2) failed", strerror(errno), pipefd_to_term[1], __LINE__);
  }

  if(dup2(pipefd_to_term[1], 2) == -1) {
    int errsv = errno;
    dup2(d,2); // Restore stderr so that we can print error message
    char msg[200];
    sprintf(msg, "could not duplicate pipefd_to_term[1]: dup2(%d,2) failed", pipefd_to_term[1]);
    error_and_exit_child(msg, strerror(errsv), pipefd_to_term[1], __LINE__);
  }
    
  if(close(pipefd_to_term[0]) == -1) {
    int errsv = errno;
    dup2(d,2);
    char msg[200];
    sprintf(msg, "could not close pipefd_to_term[0]: close(%d) failed", pipefd_to_term[0]);
    error_and_exit_child(msg, strerror(errsv), pipefd_to_term[1], __LINE__);
  }
    
  if(close(pipefd_to_term[1]) == -1) {
    int errsv = errno;
    dup2(d,2);
    char msg[200];
    sprintf(msg, "could not close pipefd_to_term[1]: close(%d) failed", pipefd_to_term[1]);
    error_and_exit_child(msg, strerror(errsv), 2, __LINE__); // Why only work properly w/ fd 1?
  }
}

int execute_with_shell()
{
  // Implement pipes
  if (pipe(pipefd_to_bash) == -1) {
    error_and_exit("unable to initialize pipefd_to_bash: pipe() failed", strerror(errno), __LINE__);
  }
  if (pipe(pipefd_to_term) == -1) {
    error_and_exit("unable to initialize pipefd_to_term: pipe() failed", strerror(errno), __LINE__);
  }

  // Fork process
  pid_t pid = fork();
  int errsv = errno;
  if (pid == -1) {
    error_and_exit("fork() failed", strerror(errsv), __LINE__);
  }

  if (pid == 0) {
    // Replace file descriptors
    replace_child_fds();

    // Exec shell
    const char* path = "/bin/bash";
    if (execl(path, path, (char*)NULL) == -1)
      error_and_exit_child("exec(\"/bin/bash\") failed", strerror(errno), pipefd_to_term[1], __LINE__);
    exit(1);
  }
  else {
    // Save to global variable
    child_pid = pid;

    // Implement signal handler for SIGPIPE
    signal(SIGPIPE, catch_sigpipe);
    // kill(child_pid, SIGINT);
    // kill(getpid(), SIGPIPE);

    // Close unused file descriptors
    if (close(pipefd_to_bash[0]) == -1) {
      int errsv = errno;
      char msg[200];
      sprintf(msg, "could not close pipefd_to_bash[0]: close(%d) failed", pipefd_to_bash[0]);
      error_and_exit(msg, strerror(errsv), __LINE__);
    }
    if (close(pipefd_to_term[1]) == -1) {
      int errsv = errno;
      char msg[200];
      sprintf(msg, "could not close pipefd_to_term[1]: close(%d) failed", pipefd_to_term[1]);
      error_and_exit(msg, strerror(errsv), __LINE__);
    }
    
    // Process input
    process_input(false);
    if (debug == true)
      fprintf(stderr, "Finished processing input (line %d)\xD\xA", __LINE__);

    const int SHELL_STATUS = print_exit_status();
    //    free(buf);
    return SHELL_STATUS;
  }
}

int main(int argc, char* argv[])
{
  // Setup argument processing
  if (argc < 2) {
    fprintf(stderr, "Missing required option: --port\n");
    exit(1);
  }
  int port = 0;
  debug = false;
  _compress = false;
  int longindex;
  static struct option long_options[] = {
    {"port", required_argument, 0, 0},
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
      exit(1);
    }
    else if (c == ':') {
      fprintf(stderr, "Missing required argument: %s\n", argv[optind-1]);
      exit(1);
    }
    if (longindex == 0)
      port = atoi(optarg);
    else if (longindex == 1)
      _compress = true;
    else if (longindex == 2)
      debug = true;
  }

  // Start code for socket
  int sockfd;
  {
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    int errsv = errno;
    if (sockfd == -1)
      error_and_exit("unable to create socket", strerror(errsv), __LINE__);
  }

  struct sockaddr_in serv_addr;
  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = INADDR_ANY;
  serv_addr.sin_port = htons(port); // htons(portno)

  // Bind address to socket
  if(bind(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1)
     error_and_exit("bind() failed", strerror(errno), __LINE__);
  
  if (debug == true)
    printf("Listening for connection...\n");
  
  // Accept connection
  if (listen(sockfd, 5) == -1)
    error_and_exit("listen() failed", strerror(errno), __LINE__);
  
  struct sockaddr client_addr;
  socklen_t client_len = sizeof(client_addr);
  {
    client_sockfd = accept(sockfd, &client_addr, &client_len);
    if (client_sockfd == -1)
      error_and_exit("accept() failed", strerror(errno), __LINE__);
  }
  
  if (debug == true)
    printf("Connection accepted!\n");

  int exit_value = execute_with_shell();
  if (close(client_sockfd) == -1)
    error_and_exit("could not close socket with client: close() failed", strerror(errno), __LINE__);

  exit(exit_value);
}
