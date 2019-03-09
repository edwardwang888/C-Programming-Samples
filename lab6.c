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

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <resolv.h>
#include <arpa/inet.h>


#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

const int B = 4275;
const int R0 = 100000;

mraa_gpio_context button;
mraa_aio_context temperature;
unsigned int period;
#define SIZE 256
char input[SIZE];
char buf[1000];
char cmd[500];
int celcius, report, log_to_file, use_tcp;
int logfd;
int server_fd, port, id;
char *host;

SSL *ssl;
X509 *cert;
SSL_CTX *ctx;

void error_and_exit(const char* message, const char* error, const int line)
{
	fprintf(stderr, "ERROR: %s at line %d: %s\xD\xA", message, line, error);
	exit(2);
}

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
	if (use_tcp)
		write(server_fd, output, strlen(output));
	else
		SSL_write(ssl, output, strlen(output));
	//write(1, output, strlen(output));
	if (log_to_file) 
		write(logfd, output, strlen(output));
	if (!use_tcp) {
		SSL_free(ssl);
		X509_free(cert);
		SSL_CTX_free(ctx);
	}
	close(logfd);
	close(server_fd);
	mraa_aio_close(temperature);
	mraa_gpio_close(button);
	//free(host);
	exit(0);
}

void process_arguments(int argc, char *argv[])
{
	// Setup argument processing
	int longindex = -1;
	id = 0;
	port = 0;
	period = 1;
	celcius = 0;
	report = 1;
	log_to_file = 0;
	static struct option long_options[] = {
		{"period", required_argument, 0, 0},
		{"log", required_argument, 0, 0},
		{"scale", required_argument, 0, 0},
		{"host", required_argument, 0, 0},
		{"id", required_argument, 0, 0},
		{0, 0, 0, 0}
	};
	
	while(1) {
		int c = getopt_long(argc, argv, ":", long_options, &longindex);
		/*
		for (int i = 0; i < argc; i++)
			printf("%s ", *(argv+i));
		printf("\n");
		*/
		if (c == -1) {
			port = atoi(*(argv+argc-1));
			break;
		}
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
		else if (longindex == 3) {
			host = optarg;
		}
		else if (longindex == 4) {
			id = atoi(optarg);
		}
	}
	if (port == 0) {
		fprintf(stderr, "Invalid/no port specified.\n");
		exit(1);
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
	//write(1, cmd, strlen(cmd));
	if (log_to_file)
		write(logfd, cmd, strlen(cmd));
}

void get_input()
{
	while (1) {
		struct pollfd fds;
		fds.fd = server_fd;
		fds.events = POLLIN;
		int c = poll(&fds, 1, 0);
		if (c == -1) {
			fprintf(stderr, "Error: poll() failed at line %d: %s\n", __LINE__, \
					strerror(errno));
			exit(2);
		}
		int bytes = 0;
		if (fds.revents == 0) 
			break;
		else {
			// Read input and add to buffer
			if (use_tcp)
				bytes = read(server_fd, input, SIZE);
			else
				bytes = SSL_read(ssl, input, SIZE);
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

void tcp_connect()
{
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
		host_info = gethostbyname(host); // Optional --host option
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
}

/* ---------------------------------------------------------- *
 * create_socket() creates the socket & TCP-connect to server *
 * ---------------------------------------------------------- */
int create_socket(char url_str[], BIO *out) {
	int sockfd;
	char hostname[256] = "";
	char      proto[6] = "";
	char      *tmp_ptr = NULL;
	char portnum[100];
	sprintf(portnum, "%d", port);
	struct hostent *host;
	struct sockaddr_in dest_addr;

	/* ---------------------------------------------------------- *
	 * Remove the final / from url_str, if there is one           *
	 * ---------------------------------------------------------- */
	if(url_str[strlen(url_str)] == '/')
		url_str[strlen(url_str)] = '\0';

	/* ---------------------------------------------------------- *
	 * the first : ends the protocol string, i.e. http            *
	 * ---------------------------------------------------------- */
	strncpy(proto, url_str, (strchr(url_str, ':')-url_str));

	/* ---------------------------------------------------------- *
	 * the hostname starts after the "://" part                   *
	 * ---------------------------------------------------------- */
	strncpy(hostname, strstr(url_str, "://")+3, sizeof(hostname));

	/* ---------------------------------------------------------- *
	 * if the hostname contains a colon :, we got a port number   *
	 * ---------------------------------------------------------- */
	if(strchr(hostname, ':')) {
		tmp_ptr = strchr(hostname, ':');
		/* the last : starts the port number, if avail, i.e. 8443 */
		strncpy(portnum, tmp_ptr+1,  sizeof(portnum));
		*tmp_ptr = '\0';
	}

	if ( (host = gethostbyname(hostname)) == NULL ) {
		BIO_printf(out, "Error: Cannot resolve hostname %s.\n",  hostname);
		abort();
	}

	/* ---------------------------------------------------------- *
	 * create the basic TCP socket                                *
	 * ---------------------------------------------------------- */
	sockfd = socket(AF_INET, SOCK_STREAM, 0);

	dest_addr.sin_family=AF_INET;
	dest_addr.sin_port=htons(port);
	dest_addr.sin_addr.s_addr = *(long*)(host->h_addr);

	/* ---------------------------------------------------------- *
	 * Zeroing the rest of the struct                             *
	 * ---------------------------------------------------------- */
	memset(&(dest_addr.sin_zero), '\0', 8);

	tmp_ptr = inet_ntoa(dest_addr.sin_addr);

	/* ---------------------------------------------------------- *
	 * Try to make the host connect here                          *
	 * ---------------------------------------------------------- */
	if ( connect(sockfd, (struct sockaddr *) &dest_addr,
				sizeof(struct sockaddr)) == -1 ) {
		BIO_printf(out, "Error: Cannot connect to host %s [%s] on port %d.\n",
				hostname, tmp_ptr, port);
	}

	return sockfd;
}

void tls_connect()
{
	char           dest_url[500];
	strcpy(dest_url, "http://");
	char portnum[100];
	sprintf(portnum, "%d", port);
	strcat(dest_url, host);
	BIO               *outbio = NULL;
	cert = NULL;
	//X509_NAME       *certname = NULL;
	const SSL_METHOD *method;
	server_fd = 0;
	//int ret, i;

	/* ---------------------------------------------------------- *
	 * These function calls initialize openssl for correct work.  *
	 * ---------------------------------------------------------- */
	OpenSSL_add_all_algorithms();
	ERR_load_BIO_strings();
	ERR_load_crypto_strings();
	SSL_load_error_strings();

	/* ---------------------------------------------------------- *
	 * Create the Input/Output BIO's.                             *
	 * ---------------------------------------------------------- */
	//BIO              *certbio = NULL;
	//certbio = BIO_new(BIO_s_file());
	outbio  = BIO_new_fp(stdout, BIO_NOCLOSE);

	/* ---------------------------------------------------------- *
	 * initialize SSL library and register algorithms             *
	 * ---------------------------------------------------------- */
	if(SSL_library_init() < 0) {
		BIO_printf(outbio, "Could not initialize the OpenSSL library !\n");
		exit(2);
	}

	/* ---------------------------------------------------------- *
	 * Set SSLv2 client hello, also announce SSLv3 and TLSv1      *
	 * ---------------------------------------------------------- */
	method = SSLv23_client_method();

	/* ---------------------------------------------------------- *
	 * Try to create a new SSL context                            *
	 * ---------------------------------------------------------- */
	if ( (ctx = SSL_CTX_new(method)) == NULL) {
		BIO_printf(outbio, "Unable to create a new SSL context structure.\n");
		exit(2);
	}

	/* ---------------------------------------------------------- *
	 * Disabling SSLv2 will leave v3 and TSLv1 for negotiation    *
	 * ---------------------------------------------------------- */
	SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2);

	/* ---------------------------------------------------------- *
	 * Create new SSL connection state object                     *
	 * ---------------------------------------------------------- */
	ssl = SSL_new(ctx);

	/* ---------------------------------------------------------- *
	 * Make the underlying TCP socket connection                  *
	 * ---------------------------------------------------------- */
	server_fd = create_socket(dest_url, outbio);

	/* ---------------------------------------------------------- *
	 * Attach the SSL session to the socket descriptor            *
	 * ---------------------------------------------------------- */
	SSL_set_fd(ssl, server_fd);

	/* ---------------------------------------------------------- *
	 * Try to SSL-connect here, returns 1 for success             *
	 * ---------------------------------------------------------- */
	if ( SSL_connect(ssl) != 1 ) {
		BIO_printf(outbio, "Error: Could not build a SSL session to: %s.\n", dest_url);
		exit(2);
	}

	/* ---------------------------------------------------------- *
	 * Get the remote certificate into the X509 structure         *
	 * ---------------------------------------------------------- */
	cert = SSL_get_peer_certificate(ssl);
	if (cert == NULL) {
		BIO_printf(outbio, "Error: Could not get a certificate from: %s.\n", dest_url);
		exit(2);
	}

	/* ---------------------------------------------------------- *
	 * extract various certificate information                    *
	 * -----------------------------------------------------------*/
	//certname = X509_NAME_new();
	//certname = X509_get_subject_name(cert);

	/* ---------------------------------------------------------- *
	 * display the cert subject here                              *
	 * -----------------------------------------------------------*/
	/*
	BIO_printf(outbio, "Displaying the certificate subject data:\n");
	X509_NAME_print_ex(outbio, certname, 0, 0);
	BIO_printf(outbio, "\n");

	BIO_printf(outbio, "Finished SSL/TLS connection with server: %s.\n", dest_url);
	*/
}

int main(int argc, char *argv[])
{
	host = malloc(1000);
	process_arguments(argc, argv);
	
	// Connect to server
	use_tcp = strcmp(*argv, "./lab4c_tls");
	if (use_tcp)
		tcp_connect();	
	else
		tls_connect();
	
	// Send ID to server
	char output[50];
	sprintf(output, "ID=%d\n", id);
	if (use_tcp)
		write(server_fd, output, strlen(output));
	else
		SSL_write(ssl, output, strlen(output));
	write(logfd, output, strlen(output));
	
	// Initialize button
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
			if (use_tcp)
				write(server_fd, output, strlen(output));
			else
				SSL_write(ssl, output, strlen(output));
			//write(1, output, strlen(output));
			if (log_to_file)
				write(logfd, output, strlen(output));
			sleep(period);
		}
	}

	return 0;
}
