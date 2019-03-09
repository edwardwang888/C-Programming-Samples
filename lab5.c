#include <mraa/gpio.h>
#include <mraa/aio.h>
#include <stdlib.h>
#include <getopt.h>
#include <sys/time.h>
#include <time.h>
#include <poll.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>

const int B = 4275;
const int R0 = 100000;

mraa_gpio_context button;
mraa_aio_context temperature;
unsigned int period;
#define SIZE 256
char input[SIZE];
char buf[1000];
char cmd[500];
int celcius, report, log_to_file;
int logfd;

void do_when_interrupted()
{
	// Print out last log and exit
	struct tm *ltime; 
	time_t rawtime;
	time(&rawtime);
	ltime = localtime(&rawtime);
	char output[200];
	sprintf(output, "%02d:%02d:%02d SHUTDOWN\n", ltime->tm_hour, ltime->tm_min, \
			ltime->tm_sec);
	printf("%s", output);
	if (log_to_file) 
		write(logfd, output, strlen(output));
	close(logfd);
	mraa_aio_close(temperature);
	mraa_gpio_close(button);
	exit(0);
}

void process_arguments(int argc, char *argv[])
{
	// Setup argument processing
	int longindex = -1;
	period = 1;
	celcius = 0;
	report = 1;
	log_to_file = 0;
	static struct option long_options[] = {
		{"period", required_argument, 0, 0},
		{"log", required_argument, 0, 0},
		{"scale", required_argument, 0, 0},
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
			period = atoi(optarg);
			if (period <= 0) {
				fprintf(stderr, "Invalid period: %s\n", optarg);
				exit(1);
			}
		}
		else if (longindex == 1) {
			log_to_file = 1;
			logfd = open(optarg, O_WRONLY|O_CREAT|O_TRUNC, S_IRUSR|S_IWUSR);
			if (logfd == -1) { 
				fprintf(stderr, "Could not open %s: %s\n", optarg, strerror(errno));
				exit(1);
			}
		}
		else if (longindex == 2) {
			if (strcmp(optarg, "C") == 0)
				celcius = 1;
		}
	}
}

void process_input()
{
	// Process command
	if (strcmp(cmd, "SCALE=F") == 0)
		celcius = 0;
	else if (strcmp(cmd, "SCALE=C") == 0)
		celcius = 1;
	else if (strncmp(cmd, "PERIOD=", 7) == 0) {
		int temp = atoi(cmd+7);
		if (temp <= 0)
			return;
		period = temp;
	}
	else if (strcmp(cmd, "STOP") == 0)
		report = 0;
	else if (strcmp(cmd, "START") == 0)
		report = 1;
	else if (strncmp(cmd, "LOG", 3) == 0) 
		;
	else if (strcmp(cmd, "OFF") == 0) {
		write(logfd, "OFF\n", 4);
		do_when_interrupted();
	}
	else	// Don't print anything
		return;

	// Print command
	strcat(cmd, "\n");
	if (log_to_file)
		write(logfd, cmd, strlen(cmd));
}

void get_input()
{
	while (1) {
		struct pollfd fds;
		fds.fd = 0;
		fds.events = POLLIN;
		int c = poll(&fds, 1, 0);
		if (c == -1) {
			fprintf(stderr, "Error: poll() failed at line %d: %s\n", __LINE__, \
				strerror(errno));
			exit(1);
		}
		int bytes = 0;
		if (fds.revents == 0) 
			break;
		else {
			// Read input and add to buffer
			bytes = read(0, input, SIZE);
			strncat(buf, input, bytes);
			// Parse buffer for commands
			int i = 0;
			while (*(buf+i) != '\0') {
				if (*(buf+i) == '\n') {
					strcpy(cmd, "");
					strncat(cmd, buf, i);
					process_input();
					// Clear command from buffer
					strcpy(buf, buf+i+1);
					i = -1;
				}
				i++;
			}
		}
	}
}

int main(int argc, char *argv[])
{
	process_arguments(argc, argv);
	button = mraa_gpio_init(60);
	temperature = mraa_aio_init(1);
	if (temperature == NULL) {
		fprintf(stderr, "Failed to initialize AIO\n");
		mraa_deinit();
		return 1;
	}

	mraa_gpio_dir(button, MRAA_GPIO_IN);
	mraa_gpio_isr(button, MRAA_GPIO_EDGE_RISING, &do_when_interrupted, NULL);

	struct tm *ltime;
	while (1) {
		// Check stdin for input
		get_input();
		if (report) {
			// Get temperature
			float value = mraa_aio_read(temperature);
			float R = 1023.0/value-1.0;
			R = R0*R;
			value = 1.0/(log(R/R0)/B+1/298.15)-273.15;
			if (!celcius)
				value = (value*9/5)+32;
			// Get local time
			time_t rawtime;
			time(&rawtime);
			ltime = localtime(&rawtime);
			// Print output
			char output[100];
			sprintf(output, "%02d:%02d:%02d %.1f\n", ltime->tm_hour, ltime->tm_min, \
					ltime->tm_sec, value);
			printf(output);
			if (log_to_file)
				write(logfd, output, strlen(output));
			sleep(period);
		}
	}
	
	return 0;
}
