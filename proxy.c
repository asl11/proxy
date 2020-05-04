/*
 * COMP 321 Project 6: Web Proxy
 *
 * This program implements a multithreaded HTTP proxy.
 *
 * Alex Li asl11 Chris Lee chl4
 */ 

#include <assert.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>

#include "csapp.h"

#define SBUFSIZE 16				/* How big we want the buffer */
#define NUMTHREADS 4			/* How many threads we want */

struct sbuf{
	int* buffer; 				/* Buffer is an int array */
	int n;  	 				/* Max num of items in buffer */
	int front;   				/* buffer[front + 1 % n] is first item */
	int last;    				/* buffer[last % n] is last item */
	pthread_mutex_t lock;   	/* mutex lock to protect info */
	pthread_cond_t  notempty;   /* Conditional to check empty buffer */
	pthread_cond_t  notfull;    /* Conditional to check full buffer */
	int items;					/* Number of items in the buffer */
};

static void	client_error(int fd, const char *cause, int err_num, 
		    const char *short_msg, const char *long_msg);
static char *create_log_entry(const struct sockaddr_in *sockaddr,
		    const char *uri, int size);
static int	parse_uri(const char *uri, char **hostnamep, char **portp,
		    char **pathnamep);

void *proxy_helper(void *vargp);
int syntax_handler(char* head, char* tail, int connfd);
void handle_sigint(int sig);
void ignore_sigpipe(int sig);
void sbuf_init(struct sbuf *sp, int n);
void sbuf_free(struct sbuf *sp);
void sbuf_insert(struct sbuf *sp, int connfd);
int  sbuf_remove(struct sbuf *sp);


//static int verbose = 1;
struct sbuf sbuf; /* Shared buffer of connected descriptors */
FILE *fptr; 	  /* Shared access to log file */

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
	pthread_t tid;

	signal(SIGINT, handle_sigint);
	signal(SIGPIPE, ignore_sigpipe);

	if (argc != 2) {
		fprintf(stderr, "usage: %s <port>\n", argv[0]);
		exit(1);
	}
	port = argv[1];
	listenfd = open_listenfd(port);

	/* Open the proxy.log file */
	fptr = fopen("proxy.log", "w"); 
	if (fptr == NULL) { 
        printf("Could not open file"); 
        return -1; 
    } 

	if (listenfd < 0)
		unix_error("open_listen error");

	/* Initialize buffer and worker threads */
	sbuf_init(&sbuf, SBUFSIZE);
	for (int i = 0; i < NUMTHREADS; i++) {
		Pthread_create(&tid, NULL, proxy_helper, NULL);
	}

	/* 
	 * Main function begins accepting and passing connfd
	 * to the shared buffer, so threads can remove 
	 * in parallel */

	while (1) {
		clientlen = sizeof(clientaddr);

		/*
		 * Call Accept() to accept a pending connection request from
		 * the client, and create a new file descriptor representing
		 * the server's end of the connection.  Assign the new file
		 * descriptor to connfd.
		 */

		if((connfd = accept(listenfd, (struct sockaddr *) &clientaddr,
			clientlen)) < 0) {
      		client_error(connfd, "Gateway Timeout", 504, "Gateway Timeout", 
      			"Accept failed");
      		close(connfd);
		}

      	sbuf_insert(&sbuf, connfd);

	}

	return 0;
}

/*
 * Requires: 
 *   vargp is a NULL input, since we don't need anything from the main process
 * 
 * Effects:
 *   This method is passed to Pthread_create, and carries out the main proxy
 *	 task. Each thread created by main will wait until the shared buffer
 *   contains an item, remove it, parse and pass the client info to the
 * 	 server, and display the server response while logging the interaction.
 * 	 This method enables concurrent accepting and servicing of client requests.
 */
void*
proxy_helper(void *vargp) {
	(void) vargp; // Otherwise won't compile.
	Pthread_detach(pthread_self());
	while (1) {

		int connfd = sbuf_remove(&sbuf);

		/* Get the client address. */
		const struct sockaddr_in clientaddr;
		socklen_t clientlen = sizeof(clientaddr);
		if (getpeername(connfd, (struct sockaddr *) &clientaddr, &clientlen) ==
			-1) {
			client_error(connfd, "Internal Server Error",
			    	500, "Internal Server Error",
			    	"Failed on getpeername");
			close(connfd);
			continue;
		}

		int currentmax = 100;
		char** lines = Malloc(sizeof(char*) * currentmax);
		int serverfd;

		/* Initialize the rio fd. */
		rio_t client_riofd;
		rio_readinitb(&client_riofd, connfd);

		/* Parse the client message. */
	    int c = 0;
	    char* line = malloc(sizeof(char) * MAXBUF);
	    while (strcmp(line, "\r\n") != 0) {
	    	/* Check if malloced space is enough. */
	    	if (c > currentmax) {
	    		lines = realloc(lines,sizeof(char*) * (currentmax + 100));
	    		currentmax += 100;
	    	}
	    	int ret_val = rio_readlineb(&client_riofd, line, MAXBUF);
	    	lines[c] = Malloc(sizeof(char) * (ret_val + 1));
			strcpy(lines[c], line);
			/* Check if the uri is longer than MAXBUF. */
	    	while (line[ret_val - 1] != '\n' && c == 0) {
                ret_val = rio_readlineb(&client_riofd, line, MAXBUF);
                lines[c] = realloc(lines[c], sizeof(char) * (strlen(lines[c]) +
                	ret_val + 1));
                lines[c] = strcat(lines[c], line);
            }
			c++;
		}

		lines = realloc(lines, sizeof(char *) * c);
		
		/* Reformat input */
		int ret_val = strlen(lines[0]);
		char* head = Malloc(ret_val + 1);
		strcpy(head, strsep(&lines[0], " "));

		char* uri = Malloc(ret_val + 1);
		strcpy(uri, strsep(&lines[0], " "));

		char* tail = Malloc(ret_val + 1);
		strcpy(tail, strsep(&lines[0], " "));

		/* Check malformed requests. */
		if (syntax_handler(head,tail,connfd) == -1) {
			/* 
			 * There was an error, and we don't want to continue 
			 * nor log this request.
			 */
			close(connfd);
			continue;
		}

		ret_val = strlen(uri);
		char* host = Malloc(ret_val + 1);
		char* port = Malloc(ret_val + 1);
		char* path = Malloc(ret_val + 1);

		/* Pass input to parse_uri. */
		parse_uri(uri, &host, &port, &path);

		/* Opening server connection. */
        if((serverfd = open_clientfd(host, port)) < 0) {
		    client_error(connfd, "Internal Server Error",
		    	500, "Internal Server Error", 
		    	"Failed to connect to given port");
        	close(connfd);
        	continue;
        }

		/* Init server response rio. */
		rio_t server_riofd;
		rio_readinitb(&server_riofd, serverfd);

		/* Reformat message for server. */
		head = strcat(head, " ");
		char* new_tail = Malloc(ret_val + 1 + strlen("\r\n"));
		strcpy(new_tail, " ");
		new_tail = strcat(new_tail, tail);

		/* Pass message to server. */
		rio_writen(serverfd, head, strlen(head));
	    rio_writen(serverfd, path, strlen(path));
	    rio_writen(serverfd, new_tail, strlen(new_tail));

	    /* Pass rest of message to server. */
		int i;
		for (i = 1; i < (c-1); i ++) {
			/* Skip the following headers. */
			if(strstr(lines[i], "Connection") != NULL) {
				continue;
			}
			if(strstr(lines[i], "Keep-Alive") != NULL) {
				continue;
			}
			if(strstr(lines[i], "Proxy-Connection") != NULL) {
				continue;
			}
			rio_writen(serverfd, lines[i], strlen(lines[i]));
		}

		/* Add close: connection if 1.1. */
		char *version = &tail[strlen(tail) - 5];
		if (strcmp(version, "1.1\r\n") == 0) {
			rio_writen(serverfd, "Connection: close\r\n", strlen("Connection:
				close\r\n"));
		}
		rio_writen(serverfd, "\r\n", strlen("\r\n"));

		char buffer[MAXLINE];

		/* Receive and parse reply. */
		int sum = 0;
	    while((i = rio_readnb(&server_riofd, buffer, MAXLINE)) > 0 ) {
	        if (rio_writen(connfd, buffer, i) != i) {
	        	printf("%s\n", buffer);
	        	client_error(connfd, "Internal Server Error",
			    	500, "Internal Server Error", "couldn't write to client");
	        	
	     	    }
	        sum += i;
	    }

	    /* Log the transaction. */
	    char* log = create_log_entry(&clientaddr, uri, sum);
	    fprintf(fptr, "%s\n", log);

	    free(lines);
	    free(line);
	    close(connfd);
	    close(serverfd);
	}
}

int 
syntax_handler(char* head, char* tail, int connfd) {
	/* This test is just so that we can check the last 3 digits of http. */
	if (strlen(tail) <= 3) {
		client_error(connfd, "Bad Request", 400, "Bad Request",
			"Malformed Get Request");
		return -1;
	}
	/* I'm dumb and can't figure out a better way to do this */
	char *version = &tail[strlen(tail) - 5];

    /* Checks http version. */
	if (strcmp(version, "1.1\r\n") != 0 &&
		strcmp(version, "1.0\r\n") != 0) {
		client_error(connfd, "HTTP Version", 505, 
			"HTTP Version Not Supported", 
			"HTTP version not 1.1 or 1.0");
		return -1;
	}
	/* Checks that request is a GET. */
	if (strcmp(head, "GET") != 0) {
		client_error(connfd, "Not Implemented", 501, 
			"Not Implemented", "Only GET requests allowed");
		return -1;
	}
	if (strcmp(tail, "HTTP/1.1\r\n") != 0 &&
			strcmp(tail, "HTTP/1.0\r\n") != 0) {
		client_error(connfd, "Bad Request", 400, 
			"Bad Request", "Malformed Get Request");
		return -1;
	}

	return 0;

}



/* 
 * Requires: signal int, probably sigint
 * 
 * Effects: Closes the file on sigint, since otherwise the file
 * won't close due to the infinite loop.
 */
void 
handle_sigint(int sig) 
{ 
	(void) sig; /* Don't need this probably. */
    /* Try to close the file */
	fflush(fptr);
	sbuf_free(&sbuf);
	if (fclose(fptr) != 0) { 
        printf("Could not close file"); 
    } 

    /* End the program */
    exit(0);
}

/* 
 * The following methods are helper methods to enable
 * shared buffer functionality. A shared buffer stores items 
 * for concurrent threads to remove and service. The main
 * process will add a connfd to the sbuffer after accepting,
 * and the concurrent threads will carry out the rest. The functions
 * are done atomically to ensure no dataraces.
 */

/* 
 * Requires:
 *   *sp is a pointer to the shared buffer. n is the number of
 *   max items that will be stored in the buffer (how big the
 *   buffer array will be initialized to). *filepointer is a
 *   file pointer so that logging may be done by the threads
 *   as well. 
 * 
 * Effects:
 *   Initializes the shared buffer and its fields.
 */
void
sbuf_init(struct sbuf *sp, int n) {
	sp->buffer = malloc (sizeof(int) * n);
	sp->n = n;
	sp->front = sp->last = 0;
	sp->items = 0;
	if (pthread_mutex_init(&(sp->lock), NULL) != 0) { 
        printf("\n mutex init has failed\n"); 
    } 
    if (pthread_cond_init(&sp->notempty, NULL) != 0) {
    	printf("\n cond init notempty has failed\n");
    }
    if (pthread_cond_init(&sp->notfull,NULL) != 0) {
    	printf("\n cond init notfull has failed\n");
    }
}

/* Requires:
 *   *sp is a pointer to the shared buffer.
 * 
 * Effects:
 *   Frees the array malloced inside the buffer.
 */
void 
sbuf_free(struct sbuf *sp) {
	Free(sp->buffer);
}

/* Requires:
 *   *sp is a pointer to the shared buffer. Connfd is the 
 *   socket id we want to put in the buffer, so that threads
 *   may remove it and service the request.
 * 
 * Effects:
 *   Inserts connfd onto the buffer atomically, updates the number
 *   of items in the buffer, and signals waiting threads.
 */
void
sbuf_insert(struct sbuf *sp, int connfd) {
	pthread_mutex_lock(&sp->lock);
	while(sp->items == sp->n) {
		pthread_cond_wait(&sp->notfull, &sp->lock);
	}
	/* At this point, there is space in the buffer to insert */
	sp->buffer[(++sp->last)%(sp->n)] = connfd;
	sp->items++;

	/* Signal threads waiting on the buffer for something to remove */
	pthread_cond_signal(&sp->notempty);
	pthread_mutex_unlock(&sp->lock);
}

/* Requires:
 *   *sp is a pointer to the shared buffer.
 * 
 * Effects:
 *   Removes a connfd from the buffer atomically, updates the 
 *   number of items in the buffer, and signals waiting threads.
 */
int 
sbuf_remove(struct sbuf *sp) {
	pthread_mutex_lock(&sp->lock);
	while(sp->items == 0) {
		pthread_cond_wait(&sp->notempty, &sp->lock);
	}
	/* At this point, there is something in the buffer to remove */
	int connfd = sp->buffer[(++sp->front)%(sp->n)];
	sp->items--;

	/* Signal threads waiting to add something to the buffer */
	pthread_cond_signal(&sp->notfull);
	pthread_mutex_unlock(&sp->lock);
	return connfd;
}

/* The following are given helper methods */

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
 *   The parameters "cause", "short_msg", and "long_msg" must point to
 * 	 properly 
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

/* 
 * Requires: a signal int
 * 
 * Effects: 
 * 	Ignores the SIGPIPE signal
 */
void
ignore_sigpipe(int sig) {
	(void) sig;
	/* We ignore sigpipe and do nothing */
}
