/*
 * COMP 321 Project 6: Web Proxy
 *
 * This program implements a multithreaded HTTP proxy.
 *
 * <Put your name(s) and NetID(s) here>
 */ 

#include <string.h>
#include <assert.h>
#include <time.h>

#include "csapp.h"

static void	client_error(int fd, const char *cause, int err_num, 
		    const char *short_msg, const char *long_msg);
static char *create_log_entry(const struct sockaddr_in *sockaddr,
		    const char *uri, int size);
static int	parse_uri(const char *uri, char **hostnamep, char **portp,
		    char **pathnamep);

/*
 * Requires:
 *   argv[1] is a string representing an unused TCP port number (in decimal).
 *
 * Effects:
 *   Opens a listening socket on the specified TCP port number.  Runs forever
 *   accepting client connections.  Echoes lines read from a connection until
 *   the connection is closed by the client.  Only accepts a new connection
 *   after the old connection is closed.
 */
int
main(int argc, char **argv)
{
	const struct sockaddr_in clientaddr;
	socklen_t clientlen;
	int connfd;
	int listenfd;
	char* port;
	int serverfd;
	char** lines = Malloc(sizeof(char*) * 100);

	if (argc != 2) {
		fprintf(stderr, "usage: %s <port>\n", argv[0]);
		exit(1);
	}
	port = argv[1];
	listenfd = open_listenfd(port);

	/* Open the proxy.log file */
	FILE *fptr = fopen("proxy.log", "w"); 
	if (fptr == NULL) { 
        printf("Could not open file"); 
        return -1; 
    } 

	if (listenfd < 0)
		unix_error("open_listen error");
	while (1) {
		clientlen = sizeof(clientaddr);

		/*
		 * Call Accept() to accept a pending connection request from
		 * the client, and create a new file descriptor representing
		 * the server's end of the connection.  Assign the new file
		 * descriptor to connfd.
		 */
		if((connfd = Accept(listenfd, (struct sockaddr *) &clientaddr, &clientlen)) < 0)
      		unix_error("Unable to accept");

      	/* Initialize the rio fd */
		rio_t client_riofd;
		rio_readinitb(&client_riofd, connfd);

	    int c = 0;
	    char line[MAXBUF];
	    while (strcmp(line, "\r\n") != 0) {
	    	int ret_val = rio_readlineb(&client_riofd, line, MAXBUF);
	    	lines[c] = Malloc(sizeof(char) * (ret_val + 1));
			strcpy(lines[c], line);
			c++;
		}
		lines = realloc(lines, sizeof(char *) * c);

		int ret_val = strlen(lines[0]);
		char* head = Malloc(ret_val + 1);
		strcpy(head, strsep(&lines[0], " "));

		char* uri = Malloc(ret_val + 1);
		strcpy(uri, strsep(&lines[0], " "));

		char* tail = Malloc(ret_val + 1);
		strcpy(tail, strsep(&lines[0], " "));

		ret_val = strlen(uri);
		char* host = Malloc(ret_val + 1);
		char* port = Malloc(ret_val + 1);
		char* path = Malloc(ret_val + 1);

		parse_uri(uri, &host, &port, &path);

		// Opening port 80 unless specified otherwise.

		if(port == NULL) {
	        if((serverfd = open_clientfd(host, "80")) < 0)
			    unix_error("Unable to connect to port 80");
	    } else {
	        if((serverfd = open_clientfd(host, port)) < 0)
			    unix_error("Unable to connect to given port");
    	}

    	head = strcat(head, " ");

    	char* new_tail = Malloc(ret_val + 1);
    	strcpy(new_tail, " ");
    	new_tail = strcat(new_tail, tail);

    	rio_writen(serverfd, head, strlen(head));
	    rio_writen(serverfd, path, strlen(path));
	    rio_writen(serverfd, new_tail, strlen(new_tail));

    	//receive reply

    	int i;
    	for (i = 1; i < c; i ++) {
    		rio_writen(serverfd, lines[i], strlen(lines[i]));
    	}
    	rio_writen(serverfd, "\r\n", strlen("\r\n"));

    	char* buffer[MAXLINE];

    	int sum = 0;
	    while((i = rio_readn(serverfd, buffer, MAXLINE)) > 0 ) {
	        rio_writen(connfd, buffer, i);
	        bzero(buffer, MAXLINE);
	        sum += i;
	    }

	    char* log = create_log_entry(&clientaddr, uri, sum);
	    printf("%s", log);
	    fwrite(log, 1, sizeof(log), fptr);

	    close(connfd);
	    close(serverfd);
	}

	fflush(fptr);
	if (fclose(fptr) != 0) { 
        printf("Could not close file"); 
        return -1; 
    } 

	return 0;
}

/*
 * Requires:
 *   The parameter "uri" must point to a properly NUL-terminated string.
 *
 * Effects:
 *   Given a URI from an HTTP proxy GET request (i.e., a URL), extract the
 *   host name, port, and path name.  Create strings containing the host name,
 *   port, and path name, and return them through the parameters "hostnamep",
 *   "portp", "pathnamep", respectively.  (The caller must free the memory
 *   storing these strings.)  Return -1 if there are any problems and 0
 *   otherwise.
 */
static int
parse_uri(const char *uri, char **hostnamep, char **portp, char **pathnamep)
{
	const char *pathname_begin, *port_begin, *port_end;

	if (strncasecmp(uri, "http://", 7) != 0)
		return (-1);

	/* Extract the host name. */
	const char *host_begin = uri + 7;
	const char *host_end = strpbrk(host_begin, ":/ \r\n");
	if (host_end == NULL)
		host_end = host_begin + strlen(host_begin);
	int len = host_end - host_begin;
	char *hostname = Malloc(len + 1);
	strncpy(hostname, host_begin, len);
	hostname[len] = '\0';
	*hostnamep = hostname;

	/* Look for a port number.  If none is found, use port 80. */
	if (*host_end == ':') {
		port_begin = host_end + 1;
		port_end = strpbrk(port_begin, "/ \r\n");
		if (port_end == NULL)
			port_end = port_begin + strlen(port_begin);
		len = port_end - port_begin;
	} else {
		port_begin = "80";
		port_end = host_end;
		len = 2;
	}
	char *port = Malloc(len + 1);
	strncpy(port, port_begin, len);
	port[len] = '\0';
	*portp = port;

	/* Extract the path. */
	if (*port_end == '/') {
		pathname_begin = port_end;
		const char *pathname_end = strpbrk(pathname_begin, " \r\n");
		if (pathname_end == NULL)
			pathname_end = pathname_begin + strlen(pathname_begin);
		len = pathname_end - pathname_begin;
	} else {
		pathname_begin = "/";
		len = 1;
	}
	char *pathname = Malloc(len + 1);
	strncpy(pathname, pathname_begin, len);
	pathname[len] = '\0';
	*pathnamep = pathname;

	return (0);
}

/*
 * Requires:
 *   The parameter "sockaddr" must point to a valid sockaddr_in structure.  The
 *   parameter "uri" must point to a properly NUL-terminated string.
 *
 * Effects:
 *   Returns a string containing a properly formatted log entry.  This log
 *   entry is based upon the socket address of the requesting client
 *   ("sockaddr"), the URI from the request ("uri"), and the size in bytes of
 *   the response from the server ("size").
 */
static char *
create_log_entry(const struct sockaddr_in *sockaddr, const char *uri, int size)
{
	struct tm result;

	/*
	 * Create a large enough array of characters to store a log entry.
	 * Although the length of the URI can exceed MAXLINE, the combined
	 * lengths of the other fields and separators cannot.
	 */
	const size_t log_maxlen = MAXLINE + strlen(uri);
	char *const log_str = Malloc(log_maxlen + 1);

	/* Get a formatted time string. */
	time_t now = time(NULL);
	int log_strlen = strftime(log_str, MAXLINE, "%a %d %b %Y %H:%M:%S %Z: ",
	    localtime_r(&now, &result));

	/*
	 * Convert the IP address in network byte order to dotted decimal
	 * form.
	 */
	Inet_ntop(AF_INET, &sockaddr->sin_addr, &log_str[log_strlen],
	    INET_ADDRSTRLEN);
	log_strlen += strlen(&log_str[log_strlen]);

	/*
	 * Assert that the time and IP address fields occupy less than half of
	 * the space that is reserved for the non-URI fields.
	 */
	assert(log_strlen < MAXLINE / 2);

	/*
	 * Add the URI and response size onto the end of the log entry.
	 */
	snprintf(&log_str[log_strlen], log_maxlen - log_strlen, " %s %d", uri,
	    size);

	return (log_str);
}

/*
 * Requires:
 *   The parameter "fd" must be an open socket that is connected to the client.
 *   The parameters "cause", "short_msg", and "long_msg" must point to properly 
 *   NUL-terminated strings that describe the reason why the HTTP transaction
 *   failed.  The string "short_msg" may not exceed 32 characters in length,
 *   and the string "long_msg" may not exceed 80 characters in length.
 *
 * Effects:
 *   Constructs an HTML page describing the reason why the HTTP transaction
 *   failed, and writes an HTTP/1.0 response containing that page as the
 *   content.  The cause appearing in the HTML page is truncated if the
 *   string "cause" exceeds 2048 characters in length.
 */
static void
client_error(int fd, const char *cause, int err_num, const char *short_msg,
    const char *long_msg)
{
	char body[MAXBUF], headers[MAXBUF], truncated_cause[2049];

	assert(strlen(short_msg) <= 32);
	assert(strlen(long_msg) <= 80);
	/* Ensure that "body" is much larger than "truncated_cause". */
	assert(sizeof(truncated_cause) < MAXBUF / 2);

	/*
	 * Create a truncated "cause" string so that the response body will not
	 * exceed MAXBUF.
	 */
	strncpy(truncated_cause, cause, sizeof(truncated_cause) - 1);
	truncated_cause[sizeof(truncated_cause) - 1] = '\0';

	/* Build the HTTP response body. */
	snprintf(body, MAXBUF,
	    "<html><title>Proxy Error</title><body bgcolor=""ffffff"">\r\n"
	    "%d: %s\r\n"
	    "<p>%s: %s\r\n"
	    "<hr><em>The COMP 321 Web proxy</em>\r\n",
	    err_num, short_msg, long_msg, truncated_cause);

	/* Build the HTTP response headers. */
	snprintf(headers, MAXBUF,
	    "HTTP/1.0 %d %s\r\n"
	    "Content-type: text/html\r\n"
	    "Content-length: %d\r\n"
	    "\r\n",
	    err_num, short_msg, (int)strlen(body));

	/* Write the HTTP response. */
	if (rio_writen(fd, headers, strlen(headers)) != -1)
		rio_writen(fd, body, strlen(body));
}

// Prevent "unused function" and "unused variable" warnings.
static const void *dummy_ref[] = { client_error, create_log_entry, dummy_ref,
    parse_uri };
