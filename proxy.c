#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "csapp.h"

#define DEBUG
#ifndef DEBUG
#define debug_printf(...) {}
#else
#define debug_printf(...) printf(__VA_ARGS__)
#endif

#ifndef VERBOSE
#define verbose_printf(...) {}
#else
#define verbose_printf(...) printf(__VA_ARGS__)
#endif

#define MAX_OBJECT_SIZE 102400 /* 100 KB */

typedef struct {
	char *header;
	int rank;
    ssize_t size;
	char *data;
} object;

int cacheSize;
int cacheLength;

//arraylist of object pointers
object **cache;

//for handling the connection
void handleConnection(int connfd);
void* newConnectionThread(void* arg);

//get the hostname and path from a URL
void parseURL(char buffer[MAXLINE], char* hostname, char* path, int *port);
//make a GET request to the server
void makeGETRequest(char* hostname, 
                    char* path,
                    int port,
                    rio_t* proxy_client,
                    int server_fd);
//copy the HTTP request from the client to the server
void copyRequest(int server_fd, rio_t* proxy_client);
//read back from the server to the client
void serveToClient(int connfd, rio_t* server_connection, 
        char* hostname, char* path);



//feature functions
//take over the connection and print the feature console
void featureConsole(int connfd, rio_t* proxy_client, char path[MAXLINE]);
//change the host, request, and port based on feature settings
void handleFeatures(char* hostname, char* path, int* port);

/* 
 * Cache Functions
 */
//add the object to the cache
void add_object(object *o);
//evict sufficient blocks to add an object of size size to the cache
void make_space(int size);
//query the cache for a header of size 
int query_cache(char *header);
//grow the arraylist
void grow_cache();

/*****
 * Features structure
 *  Holds information about the features enabled/disabled
 *  Locked/unlocked by features_mutex
 *****/
pthread_mutex_t features_mutex;
struct features_t
{
    //nope mode: redirects all requests to a picture that says "NOPE"
    int nope;
    //rickroll mode: redirect all youtube watch requests to rick astley
    int rickroll;
};
struct features_t ft_config;;

//@TODO: some way to deal with worst-case thread pileups
//       I have some ideas


int main (int argc, char *argv []){
	//@TODO: sigactions
	signal(SIGPIPE, SIG_IGN);
	
	int listenfd, connfd, port;
    socklen_t clientlen;
	struct sockaddr_in clientaddr;
	cache = malloc(sizeof(object *) * 10);
	cacheLength = 0;
	cacheSize = 0; 
	if(argc != 2){
		fprintf(stderr,"Usage %s <port>\n",argv[0]);
		exit(1);
	}
	port = atoi(argv[1]);
	listenfd = open_listenfd(port);

    //init features
    //don't lock because it doesn't matter here (no threads)
    ft_config.nope = 0;
    ft_config.rickroll = 0;


    //initialize mutexes
    pthread_mutex_init(&features_mutex, NULL);

	while(1) {

		pthread_t tid;
		clientlen = sizeof(clientaddr);
		connfd = accept(listenfd , (SA *)&clientaddr, &clientlen);

#ifdef SEQUENTIAL
        handleConnection(connfd);
#else
        int* fd_place = malloc(sizeof(int));
        *fd_place = connfd;
        pthread_create(&tid,
                       NULL,
                       newConnectionThread,
                       (void*)fd_place);
#endif
    }
}
void* newConnectionThread(void* arg)
{
    pthread_detach(pthread_self());
	int connfd = *(int*)arg;
    free(arg);
    handleConnection(connfd);
}

void handleConnection(int connfd){
    char buffer[MAXLINE];
    memset(buffer, '\0', MAXLINE*sizeof(char));
    rio_t proxy_client;
    rio_readinitb(&proxy_client, connfd);

    int server_fd;
    rio_t server_connection;

    //get the first line of the request into the buffer
    rio_readlineb(&proxy_client, buffer, MAXLINE);
    if(strncmp(buffer, "GET ", 4) == 0)
    {
        //we've got a get request
        debug_printf("Executing a GET request\n");

        //parse the URL (hostname, path, and port) from the first line
        char hostname[MAXLINE];
        char path[MAXLINE];
        int port=80;
        parseURL(buffer, hostname, path, &port);

        //if we're trying to access the features console
        //  then call the feature handler and end the function
        if((strcmp(hostname, "proxy-configurator") == 0))
        {
            //manage the features
            featureConsole(connfd, &proxy_client, path);
            //and done
            return;
        }

        //do silly things based on the status of features
        handleFeatures(hostname, path, &port);

        
        //some debug statements
        debug_printf("Trying to contact hostname %s on port %d\n",
                      hostname, port);
        debug_printf("I'll ask him for the path '%s'\n", path);


        //open the connection to the remote server
        if((server_fd = open_clientfd(hostname, port)) < 0)
        {
            char errorbuf[] = "HTTP 404 NOTFOUND\r\n\r\n404 Not Found\r\n";
            rio_writen(connfd, errorbuf, strlen(errorbuf));
            close(connfd);
            return;
        }
        rio_readinitb(&server_connection, server_fd);

        //now, make the GET request to the server
        makeGETRequest(hostname, path, port,
                       &proxy_client, server_fd);


        //now read from the server back to the client
        serveToClient(connfd, &server_connection, hostname, path);

        //clean up
        close(server_fd);
        debug_printf("Closed connection to %s%s\n", hostname, path);
    }
    close(connfd);
}

//parse a URL and set the hostname and path into the given buffers
void parseURL(char buffer[MAXLINE], char* hostname, char* path, int *port)
{
    //now let's copy out the hostname

    memset(hostname, '\0', MAXLINE*sizeof(char));
    memset(path, '\0', MAXLINE*sizeof(char));

    int i = 11; //first character after "GET http://"
    while(buffer[i] == '/')
    {
        i++; //this accounts for GET/POST and https
    }
    while(buffer[i] &&
            (buffer[i] != '/') &&
            (buffer[i] != ':'))
    {
        hostname[i-11] = buffer[i];
        i++;
    }
    if(buffer[i] == ':')
    {
        sscanf(&buffer[i+1], "%d%s", port, path);
    }
    else
    {
        sscanf(&buffer[i], "%s", path);
    }

    hostname[i-11] = '\0';
}

//read the request from the 

//copy request headers from the client to the server
//guaranteed to be complete lines
void copyRequest(int server_fd, rio_t* proxy_client)
{
    char buffer[MAXLINE];
    while(rio_readlineb(proxy_client, buffer, MAXLINE) && 
            strncmp(buffer, "\r", 1))
    {
        if(strncmp(buffer, "Proxy-Connection: ", 18))
        {
            rio_writen(server_fd, buffer, strlen(buffer));
            verbose_printf("->\t%s", buffer);
        }

    }
}

void makeGETRequest(char* hostname, 
                    char* path,
                    int port, 
                    rio_t* proxy_client,
                    int server_fd)
{
    //make the GET request
    rio_writen(server_fd, "GET ", strlen("GET "));
    rio_writen(server_fd, path, strlen(path));
    rio_writen(server_fd, " ", strlen(" "));
    rio_writen(server_fd, "HTTP/1.0", strlen("HTTP/1.0"));
    rio_writen(server_fd, "\r\n", strlen("\r\n"));

    verbose_printf("->\t%s%s HTTP/1.0 \r\n", "GET ", path);


    copyRequest(server_fd, proxy_client);
    //
    //finish the request
    rio_writen(server_fd, "\r\n", strlen("\r\n"));
}

void serveToClient(int connfd, rio_t* server_connection, 
        char* hostname, char* path)
{
    object* cacheobj = malloc(sizeof(object));
    //read into here, then copy the right amount into dynamic memory
    char tempbuffer[MAX_OBJECT_SIZE];
    ssize_t bufferpos=0;


    char buffer[MAXLINE];

    ssize_t n = 0; //number of bytes
    while(((n=rio_readlineb(server_connection, buffer, MAXLINE)) != 0) && 
            buffer[0] != '\r')
    {
        verbose_printf("<-\t%s", buffer);
        if(rio_writen(connfd, buffer, strlen(buffer)) < 0)
        {
            printf("Write error\n");
            break;
        }
        if((bufferpos + n) >= MAX_OBJECT_SIZE)
        {
            //null out the object to signify that it's too big
            free(cacheobj);
            cacheobj = NULL;
        }
        else if(cacheobj)
        {
            memcpy(&tempbuffer[bufferpos], buffer, n);
            bufferpos += n;
        }
    }
    rio_writen(connfd, "\r\n", strlen("\r\n"));
    
    //@TODO: cache this

    while((n=rio_readnb(server_connection, buffer, MAXLINE)) >0)
    {
        if(rio_writen(connfd, buffer, n) < 0)
        {
            printf("Error writing from %s%s\n", hostname, path);
            //error on write
            return;
        }

        if((bufferpos + n) >= MAX_OBJECT_SIZE)
        {
            //null out the object to signify that it's too big
            free(cacheobj);
            cacheobj = NULL;
        }
        else if(cacheobj)
        {
            memcpy(&tempbuffer[bufferpos], buffer, n);
            bufferpos += n;
        }
        verbose_printf("<-\t%s", buffer);
        memset(buffer, '\0', MAXLINE*sizeof(char));
    }

    if(cacheobj)
    {
        debug_printf("Built up a cache buffer of length %u\n", bufferpos-1);

        cacheobj->size = bufferpos - 1;
        cacheobj->data = malloc(bufferpos-1 * sizeof(char));
        memcpy(cacheobj->data, tempbuffer, bufferpos-1);

        //@TODO: uncomment
        //add_object(cacheobj);

        //@TODO: delete
        free(cacheobj->data);
        free(cacheobj);
    }
    else
    {
        debug_printf("Object was too big for cache, didn't cache it\n");
    }
}


/*************
 ** Feature Functions
 ** Not related to the core functionality of the proxy
 *************/
void handleFeatures(char* hostname, char* path, int* port)
{
    //@TODO: reader/writer lock on features
    struct features_t features;

    pthread_mutex_lock(&features_mutex);
    memcpy(&features, &ft_config, sizeof(struct features_t));
    pthread_mutex_unlock(&features_mutex);

    if(features.nope)
    {
        printf("Lol, nope.\n");
        sprintf(hostname, "farm6.staticflickr.com");
        sprintf(path, "/5258/5582667252_b3b46db1ec_b.jpg");
        *port=80;
    }
    if(features.rickroll)
    {
        //redirect any youtube link to rickroll
        if( ((strcmp(hostname, "youtube.com") == 0)
            || (strcmp(hostname, "www.youtube.com") == 0))
           && (strncmp(path, "/watch", 6) == 0))
        {
            sprintf(path, "/watch?v=oHg5SJYRHA0\0");
        }
    }
}
void featureConsole(int connfd, rio_t* proxy_client, char path[MAXLINE])
{
    //first, get all the headers in the client's request.
    //we can discard most or all of them
    char buffer[MAXLINE];
    while(rio_readlineb(proxy_client, buffer, MAXLINE) && 
            strncmp(buffer, "\r", 1)){};


    
    if(strncmp(path, "/set/nope", 9)==0)
    {
        printf("Setting nope mode\n");
        //set nope
        pthread_mutex_lock(&features_mutex);
        ft_config.nope = 1;
        pthread_mutex_unlock(&features_mutex);

        //and return to the status page
        char header[] = "HTTP/1.0 302 Found\r\n"
                          "Location: /\r\n\r\n";
        rio_writen(connfd, header, strlen(header));
    }
    else if(strncmp(path, "/set/unnope", 11)==0)
    {
        printf("Unsetting nope mode\n");
        //clear nope
        pthread_mutex_lock(&features_mutex);
        ft_config.nope = 0;
        pthread_mutex_unlock(&features_mutex);

        //and return to the status page
        char header[] = "HTTP/1.0 302 Found\r\n"
                          "Location: /\r\n\r\n";
        rio_writen(connfd, header, strlen(header));
    }
    else if(strncmp(path, "/set/rickroll", 8)==0)
    {
        printf("Setting rickroll mode\n");
        //set rickroll
        pthread_mutex_lock(&features_mutex);
        ft_config.rickroll = 1;
        pthread_mutex_unlock(&features_mutex);

        //and return to the status page
        char header[] = "HTTP/1.0 302 Found\r\n"
                          "Location: /\r\n\r\n";
        rio_writen(connfd, header, strlen(header));
    }
    else if(strncmp(path, "/set/norickroll", 10)==0)
    {
        printf("Unsetting rickroll mode\n");
        //clear rickroll
        pthread_mutex_lock(&features_mutex);
        ft_config.rickroll = 0;
        pthread_mutex_unlock(&features_mutex);

        //and return to the status page
        char header[] = "HTTP/1.0 302 Found\r\n"
                          "Location: /\r\n\r\n";
        rio_writen(connfd, header, strlen(header));
    }
    else if(strncmp(path, "/info", 5)==0)
    {
        char header[] = "HTTP/1.0 200 OK\r\n"
                          "Content-Type: text/html\r\n\r\n";
        rio_writen(connfd, header, strlen(header));

        char response[] = "<title>Proxy Diagnostic Page</title>"
                          "<h1>Cache Diagnostics</h1><hr />";
        rio_writen(connfd, response, strlen(response));
    }
    //other conditions here
    else
    {
        char header[] = "HTTP/1.0 200 OK\r\n"
                          "Content-Type: text/html\r\n\r\n";
        rio_writen(connfd, header, strlen(header));

        char response[] = "<title>Features Manager</title>"
                          "<h1>Features Manager</h1><hr />"
                          "<b>The settings:</b><br /><br />";
        rio_writen(connfd, response, strlen(response));



        char dynamiccontent[MAXLINE];
        memset(dynamiccontent, '\0', MAXLINE*sizeof(char));
        
        pthread_mutex_lock(&features_mutex);

        snprintf(dynamiccontent, MAXLINE, 
                                "<table style='border-left: 1px black solid' >"
                                "<tr><td>NOPE Mode:</td><td>%s</td></tr>"
                                "<tr><td>Rickroll:</td><td>%s</td></tr>"
                                "</table>",
                                (ft_config.nope)?"on":"off",
                                (ft_config.rickroll)?"on":"off");

        pthread_mutex_unlock(&features_mutex);
        rio_writen(connfd, dynamiccontent, strlen(dynamiccontent));

        char options[] = "<style>"
                         "body{"
                         "  font-family: sans-serif;"
                         "}"
                         ".options td{ "
                         "  border: 1px black solid;"
                         "  margin-right: 10px;"
                         "  background-color: lightgray;"
                         "  font-weight: bold;"
                         "}"
                         ".options a:link, a:visited{"
                         "  text-decoration: none;"
                         "  color: black;"
                         "}"
                         ".options a:hover{"
                         "  text-decoration: none;"
                         "  color: red;"
                         "}"
                         "</style>"
                         "<br /><table class='options'>"
                         "<tr>"
                         "  <td><a href='/set/nope'>Engage NOPE</a></td>"
                         "  <td><a href='/set/unnope'>Disengage NOPE</a></td>"
                         "</tr>"
                         "<tr>"
                         "  <td><a href='/set/rickroll'>"
                         "      Engage Rickroll"
                         "  </a></td>"
                         "  <td><a href='/set/norickroll'>"
                         "      Disengage Rickroll"
                         "  </a></td>"
                         "</tr>"
                         "<tr>"
                         "</tr>"
                         "</table>";
        rio_writen(connfd, options, strlen(options));

        char response2[] = "<br /><hr /><i>This proxy server written by Jeff Cooper and "
                   "Prashant Sridhar, and powered by Caffeine.</i>";
        rio_writen(connfd, response2, strlen(response2));
    }

    close(connfd);
}
