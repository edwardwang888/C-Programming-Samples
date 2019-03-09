#define _POSIX_SOURCE
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <poll.h>
#include <getopt.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>

#include <sys/types.h>
#include <signal.h>

struct termios termios_save;
int read_pipe_to_term;

void reset_terminal()
{
  int c = tcsetattr(0, TCSANOW, &termios_save);
  int errsv = errno;
  if (c == -1) {
    fprintf(stderr, "ERROR: tcsetattr() failed: "
	    "could not reset file descriptor (0): %s\xD\xA", strerror(errsv));
    exit(1);
  }
}

void error_and_exit(const char* message, const char* error, const int line)
{
  reset_terminal();
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
  if (wait(&wstatus) == -1)
    error_and_exit("wait() failed", strerror(errno), __LINE__);
  const int SHELL_SIGNAL = wstatus & 0x007f;
  const int SHELL_STATUS = (wstatus & 0xff00)>>8;
  fprintf(stderr, "SHELL EXIT SIGNAL=%d STATUS=%d\xD\xA", SHELL_SIGNAL, SHELL_STATUS);
  return SHELL_STATUS;
}

void catch_sigpipe()
{
  // Read remaining input and then exit
  struct pollfd shell = {read_pipe_to_term, POLLIN, 0};
  const int SIZE = 256;
  char* buf = malloc(SIZE);
  int errsv = errno;
  if (buf == NULL)
    error_and_exit("could not initialize buf: malloc() failed", strerror(errsv), __LINE__);
  while(1) {
    int c = poll(&shell, 1, 0);
    // printf("Return value of poll: %d\xD\xA", c);
    int errsv = errno;
    if (c < 0) {
      error_and_exit("poll() failed", strerror(errsv), __LINE__);
    }
    else if (c == 0) // No more input left; perform shutdown sequence
      goto end;
    else {
      //if (shell.revents & POLLIN != 0)
	

      // Poll succeeded, so read from shell
      int bytes_read = read(read_pipe_to_term, buf, SIZE);
      int errsv = errno;
      if (bytes_read < 0)
	error_and_exit("could not read from shell: read() failed", strerror(errsv), __LINE__);

      if (bytes_read == 0)
	goto end;

      // Write buffer
      for (int i = 0; i < bytes_read; i++) {
	switch(*(buf+i)) {
	case 13:
	  // Receive <CR> from shell, print to stdout unmodified
	  if (write(1, buf+i, 1) == -1)
	    error_and_exit("could not write to stdout: write(1) failed", \
			   strerror(errno), __LINE__);
	  break;

	case 10:
	  // Receive <LF> from shell, map to <CR><LF> for stdout
	  if (write(1, "\xD\xA", 2) == -1)
	    error_and_exit("could not write to stdout: write(1) failed", \
			   strerror(errno), __LINE__);
	  break;

	default:
	  if (write(1, buf+i, 1) == -1)
	    error_and_exit("could not write to stdout: write(1) failed", \
			   strerror(errno), __LINE__);
	  break;
	}    
      }
    }
    end: ;
    reset_terminal();
    const int SHELL_STATUS = print_exit_status();
    free(buf);
    exit(SHELL_STATUS);
  }
}

int execute_without_shell()
{
  // Read input
  const int SIZE = 256;
  char* buf = malloc(SIZE);
  int errsv = errno;

  if (buf == NULL)
    error_and_exit("malloc() of buf failed", strerror(errsv), __LINE__);
  
  while(1) {
    int bytes_read = read(0, buf, SIZE);
    int errsv = errno;
    if (bytes_read < 0)
      error_and_exit("read(0) failed", strerror(errsv), __LINE__);
    for (int i = 0; i < bytes_read; i++) {
      switch(*(buf+i)) {
      case 4: // EOT
	goto end; break;
      case 13: case 10: // CR or LF
	if (write(1, "\xD\xA", 2) == -1)
	  error_and_exit("write(1) failed", strerror(errno), __LINE__);
	break;
      default:
	if (write(1, buf+i, 1) == -1)
	  error_and_exit("write(1) failed", strerror(errno), __LINE__);
	break;
      }    
    }
  }

  end:
    free(buf);
    return 0;
}

int execute_with_shell()
{
  // Implement pipes
  int pipefd_to_bash[2], pipefd_to_term[2];
  if (pipe(pipefd_to_bash) == -1) {
    error_and_exit("unable to initialize pipefd_to_bash: pipe() failed", strerror(errno), __LINE__);
  }
  if (pipe(pipefd_to_term) == -1) {
    error_and_exit("unable to initialize pipefd_to_term: pipe() failed", strerror(errno), __LINE__);
  }

  read_pipe_to_term = pipefd_to_term[1];

  // Fork process
  pid_t pid = fork();
  int errsv = errno;
  if (pid == -1) {
    error_and_exit("fork() failed", strerror(errsv), __LINE__);
  }
  
  // Implement signal handler for SIGPIPE
  signal(SIGPIPE, catch_sigpipe);
  // kill(getpid(), SIGPIPE);

  if (pid == 0) {
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
      char *msg = malloc(0); // Also can do this with char[]
      free(msg);
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
      char *msg = malloc(0);
      free(msg);
      sprintf(msg, "could not duplicate pipefd_to_term[1]: dup2(%d,2) failed", pipefd_to_term[1]);
      error_and_exit_child(msg, strerror(errsv), pipefd_to_term[1], __LINE__);
    }
    
    if(close(pipefd_to_term[0]) == -1) {
      int errsv = errno;
      dup2(d,2);
      char *msg = malloc(0);
      free(msg);
      sprintf(msg, "could not close pipefd_to_term[0]: close(%d) failed", pipefd_to_term[0]);
      error_and_exit_child(msg, strerror(errsv), pipefd_to_term[1], __LINE__);
    }
    
    if(close(pipefd_to_term[1]) == -1) {
      int errsv = errno;
      dup2(d,2);
      char *msg = malloc(0);
      free(msg);
      sprintf(msg, "could not close pipefd_to_term[1]: close(%d) failed", pipefd_to_term[1]);
      error_and_exit_child(msg, strerror(errsv), 2, __LINE__); // Why only work properly w/ fd 1?
    }
    
    // Exec shell
    const char* path = "/bin/bash";
    if (execl(path, path, (char*)NULL) == -1)
      error_and_exit_child("exec(\"/bin/bash\") failed", strerror(errno), pipefd_to_term[1], __LINE__);
    exit(1);
  }
  else {
    // Set up poll
    if (close(pipefd_to_bash[0]) == -1) {
      int errsv = errno;
      char *msg = malloc(0);
      free(msg);
      sprintf(msg, "could not close pipefd_to_bash[0]: close(%d) failed", pipefd_to_bash[0]);
      error_and_exit(msg, strerror(errsv), __LINE__);
    }
    if (close(pipefd_to_term[1]) == -1) {
      int errsv = errno;
      char *msg = malloc(0); free(msg);
      sprintf(msg, "could not close pipefd_to_term[1]: close(%d) failed", pipefd_to_term[1]);
      error_and_exit(msg, strerror(errsv), __LINE__);
    }
    struct pollfd fds[2];
    
    // Set keyboard poll
    fds[0].fd = 0;
    fds[0].events = POLLIN;
    
    // Set bash poll
    fds[1].fd = pipefd_to_term[0];
    fds[1].events = POLLIN;

    // Read input
    const int SIZE = 256;
    char* buf = malloc(SIZE);
    int errsv = errno;
    if (buf == NULL)
      error_and_exit("could not initialize buf: malloc() failed", strerror(errsv), __LINE__);

    while(1) {
      int c = poll(fds,2,0);
      int errsv = errno;
      if (c < 0) {
	error_and_exit("poll() failed", strerror(errsv), __LINE__);
      }
      else if (c > 0) {
	// Check which poll succeeded
	int bytes_read;
	if (fds[0].revents != 0)
	  bytes_read = read(0, buf, SIZE);
	else if (fds[1].revents != 0)
	  bytes_read = read(pipefd_to_term[0], buf, SIZE);
	else { // This should never occur
	  fprintf(stderr, "Error in poll: no ready file descriptor found.\n");
	}
	int errsv = errno;
	if (bytes_read < 0) {
	  if (fds[0].revents != 0)
	    error_and_exit("could not read from stdin: read() failed", strerror(errsv), __LINE__);
	  else
	    error_and_exit("could not read from pipefd_to_term[0]: "
			   "read() failed", strerror(errsv), __LINE__);
	}
	
	// Check for EOF; this should never happen for the keyboard
	if (bytes_read == 0) {
	  if (fds[0].revents != 0) // This should never occur
	    fprintf(stderr, "Error!! read(0) indicates return value of zero.\n");
	  //	  fprintf(stderr, "read() on shell returned 0\xD\xA");
	  goto end;
	}

	// Write buffer
	for (int i = 0; i < bytes_read; i++) {
	  switch(*(buf+i)) {
	  case 3:
	    if (kill(pid, SIGINT) == -1) {
	      int errsv = errno;
	      char* msg = malloc(0); free(msg);
	      sprintf(msg, "sending SIGINT to process %d: kill() failed", pid);
	      error_and_exit(msg, strerror(errsv), __LINE__);
	    }
	    break;
	  case 4:
	    // Close pipe to shell
	    if (close(pipefd_to_bash[1]) == -1) {
	      int errsv = errno;
	      char* msg = malloc(0); free(msg);
	      sprintf(msg, "could not close pipefd_to_bash[1]: close(%d) failed", pipefd_to_bash[1]);
	      error_and_exit(msg, strerror(errsv), __LINE__);
	    }
	    break;

	  case 13:
	    // Receive <CR> from keyboard, map to <CR><LF> for stdout, <LF> for bash
	    if (fds[0].revents != 0) {
	      if (write(1, "\xD\xA", 2) == -1) {
		error_and_exit("could not write to stdout: write(1) failed", \
			       strerror(errno), __LINE__);
	      }
	      if (write(pipefd_to_bash[1], "\xA", 1) == -1) {
		int errsv = errno;
		char* msg = malloc(0); free(msg);
		sprintf(msg, "could not write to pipefd_to_bash[1]: "
			"write(%d) failed", pipefd_to_bash[1]);
		error_and_exit(msg, strerror(errsv), __LINE__);
	      }
	    }
	    // Receive <CR> from shell, write to stdout unmodified
	    else if (fds[1].revents != 0) {
	      if (write(1, buf+i, 1) == -1)
		error_and_exit("could not write to stdout: write(1) failed", \
			       strerror(errno), __LINE__);
	    }
	    break;

	  case 10:
	    // Receive <LF> from keyboard, map to <CR><LF> for stdout, <LF> for bash
	    if (fds[0].revents != 0) {
	      if (write(1, "\xD\xA", 2) == -1) {
		error_and_exit("could not write to stdout: write(1) failed", \
			       strerror(errno), __LINE__);
	      }
	      if (write(pipefd_to_bash[1], buf+i, 1) == -1) {
		int errsv = errno;
		char* msg = malloc(0); free(msg);
		sprintf(msg, "could not write to pipefd_to_bash[1]: "
			"write(%d) failed", pipefd_to_bash[1]);
		error_and_exit(msg, strerror(errsv), __LINE__);
	      }
	    }
	    // Receive <LF> from shell, map to <CR><LF> for stdout
	    else {
	      if (write(1, "\xD\xA", 2) == -1)
		error_and_exit("could not write to stdout: write(1) failed", \
			       strerror(errno), __LINE__);
	    }
	    break;

	  default:
	    if (write(1, buf+i, 1) == -1)
	      error_and_exit("could not write to stdout: write(1) failed", \
			     strerror(errno), __LINE__);
	    if (fds[0].revents != 0)
	      if (write(pipefd_to_bash[1], buf+i, 1) == -1) {
		int errsv = errno;
		char *msg = malloc(0); free(msg);
		sprintf(msg, "could not write to pipefd_to_bash[1]: "
			"write(%d) failed", pipefd_to_bash[1]);
		error_and_exit(msg, strerror(errsv), __LINE__);
	      }
	    break;
	  }    
	}
      }
      // Reset revents
      fds[0].revents = 0;
      fds[1].revents = 0;
    }
    // In theory, the program would never reach this section
    printf("Program never reaches here\xD\xA");
    wait(NULL);

  end: ;
    /*    int wstatus;    
    if (wait(&wstatus) == -1)
      error_and_exit("wait() failed", strerror(errno), __LINE__);
    const int SHELL_SIGNAL = wstatus & 0x007f;
    const int SHELL_STATUS = (wstatus & 0xff00)>>8;
    fprintf(stderr, "SHELL EXIT SIGNAL=%d STATUS=%d\xD\xA", SHELL_SIGNAL, SHELL_STATUS);
    */
    const int SHELL_STATUS = print_exit_status();
    free(buf);
    return SHELL_STATUS;
  }
}

int main(int argc, char* argv[])
{
  // Save terminal attributes (termios_save declared globally)
  int c = tcgetattr(0, &termios_save);
  
  // Set new attributes
  struct termios termios_new = termios_save;
  termios_new.c_iflag = ISTRIP;
  termios_new.c_oflag = 0;
  termios_new.c_lflag = 0;
  c = tcsetattr(0, TCSANOW, &termios_new);
  int errsv = errno;
  if (c == -1) {
    error_and_exit("could not set stdin: tcsetattr() failed", strerror(errsv), __LINE__);
  }

  // Setup argument processing
  int longindex;
  bool shell = false;
  static struct option long_options[] = {
    {"shell", no_argument, 0, 0},
    {0, 0, 0, 0}
  };

  while(1) {
    int c = getopt_long(argc, argv, ":", long_options, &longindex);
    if (c == -1)
      break;
    else if (c == '?') {
      fprintf(stderr, "Unrecognized option: %s\xD\xA", argv[optind-1]);
      reset_terminal();
      exit(1);
    }
    shell = true;
  }

  int exit_value;
  if (shell == true)
    exit_value = execute_with_shell();
  else
    exit_value = execute_without_shell();

  // Reset terminal attributes
  reset_terminal();
  exit(exit_value);
}
