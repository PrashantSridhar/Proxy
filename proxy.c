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

#define VERBOSE
#ifndef VERBOSE
#define verbose_printf(...) {}
#else
#define verbose_printf(...) printf(__VA_ARGS__)
#endif



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
//make a POST request to the server
//currently experimental
void makePOSTRequest(char* hostname, 
                    char* path,
                    int port,
                    rio_t* proxy_client,
                    int server_fd);
//copy the HTTP request from the server to the client
void copyRequest(int server_fd, rio_t* proxy_client);
//read back from the server to the client
void serveToClient(int connfd, rio_t* server_connection, 
        char* hostname, char* path);



//easter egg functions
//take over the connection and print the easter egg console
void easterEgg(int connfd, rio_t* proxy_client, char path[MAXLINE]);
//change the host, request, and port based on egg settings
void handleEasterEgg(char* hostname, char* path, int* port);
void fbsniffReadback(int connfd, rio_t* server_connection);
//this version ignores Accept-Encoding: gzip
void copyRequestNoGzip(int server_fd, rio_t* proxy_client);

/******
 * Mutexes
 ******/

pthread_mutex_t easteregg_mutex;


/*****
 * Easter Egg structure
 *  Holds information about the easter egg features
 *  Locked/unlocked by easteregg_muted
 *****/
struct ee_features_t
{
    //nope mode: redirects all requests to a picture that says "NOPE"
    int nope;
    //rickroll mode: redirect all youtube watch requests to rick astley
    int rickroll;
};
struct ee_features_t ee_config;

//@TODO: some way to deal with worst-case thread pileups
//       I have some ideas


int main (int argc, char *argv []){
	//@TODO: sigactions
	signal(SIGPIPE, SIG_IGN);
	int listenfd, connfd, port;
    socklen_t clientlen;
	struct sockaddr_in clientaddr;
	
	if(argc != 2){
		fprintf(stderr,"Usage %s <port>\n",argv[0]);
		exit(1);
	}
	port = atoi(argv[1]);
	listenfd = open_listenfd(port);

    //init easteregg features
    //don't lock because it doesn't matter here (no threads)
    ee_config.nope = 0;
    ee_config.rickroll = 0;


    //initialize mutexes
    pthread_mutex_init(&easteregg_mutex, NULL);

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

        ////if we're trying to access the easter egg console,
        ////  then call the easter egg handler and end the function
        //if((strcmp(hostname, "proxy-configurator") == 0))
        //{
        //    //this is some fun easter-egg-ing that we can do
        //    easterEgg(connfd, &proxy_client, path);
        //    //and done
        //    return;
        //}

        ////do silly things based on the status of easter eggs
        //handleEasterEgg(hostname, path, &port);

        
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

//copy request headers from the client to the server
//guaranteed to be complete lines
void copyRequest(int server_fd, rio_t* proxy_client)
{
    char buffer[MAXLINE];
    while(rio_readlineb(proxy_client, buffer, MAXLINE) && 
            strncmp(buffer, "\r", 1))
    {
        rio_writen(server_fd, buffer, strlen(buffer));
        verbose_printf("->\t%s", buffer);
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
    char buffer[MAXLINE];
    while(rio_readlineb(server_connection, buffer, MAXLINE) != 0 && 
            buffer[0] != '\r')
    {
        verbose_printf("<-\t%s", buffer);
        if(rio_writen(connfd, buffer, strlen(buffer)) < 0)
        {
            printf("Write error\n");
            break;
        }
    }
    rio_writen(connfd, "\r\n", strlen("\r\n"));
    
    //@TODO: cache this

    /****************
          ____  _____ _____  ____  __ _____ 
         / __ \|  ___|_ _\ \/ /  \/  | ____|
        / / _` | |_   | | \  /| |\/| |  _|  
       | | (_| |  _|  | | /  \| |  | | |___ 
        \ \__,_|_|   |___/_/\_\_|  |_|_____|
         \____/   This is the root of the proxy's slowness.
         Figure out what's wrong with these reads (it goes through one
         iteration of the loop, then hangs until the connection times out,
         then does another iteration), and the proxy will work just fine.
   ************/


    ssize_t n = 0; //number of bytes
    while((n=rio_readnb(server_connection, buffer, 1024)) >0)
    {
        debug_printf("Read %u bytes\n", n);
        if(rio_writen(connfd, buffer, n) < 0)
        {
            printf("Error writing from %s%s\n", hostname, path);
            //error on write
            return;
        }
        else
        {
            debug_printf("\t Wrote %u bytes\n", n);
        }
        verbose_printf("<-\t%s", buffer);
        memset(buffer, '\0', MAXLINE*sizeof(char));
    }
}


/*************
 ** Easter Egg functions
 ** Not related to the core functionality of the proxy
 *************/
void handleEasterEgg(char* hostname, char* path, int* port)
{
    //@TODO: reader/writer lock on easteregg
    struct ee_features_t features;

    pthread_mutex_lock(&easteregg_mutex);
    memcpy(&features, &ee_config, sizeof(struct ee_features_t));
    pthread_mutex_unlock(&easteregg_mutex);

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
void easterEgg(int connfd, rio_t* proxy_client, char path[MAXLINE])
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
        pthread_mutex_lock(&easteregg_mutex);
        ee_config.nope = 1;
        pthread_mutex_unlock(&easteregg_mutex);

        //and return to the status page
        char header[] = "HTTP/1.0 302 Found\r\n"
                          "Location: /\r\n\r\n";
        rio_writen(connfd, header, strlen(header));
    }
    else if(strncmp(path, "/set/unnope", 11)==0)
    {
        printf("Unsetting nope mode\n");
        //clear nope
        pthread_mutex_lock(&easteregg_mutex);
        ee_config.nope = 0;
        pthread_mutex_unlock(&easteregg_mutex);

        //and return to the status page
        char header[] = "HTTP/1.0 302 Found\r\n"
                          "Location: /\r\n\r\n";
        rio_writen(connfd, header, strlen(header));
    }
    else if(strncmp(path, "/set/rickroll", 8)==0)
    {
        printf("Setting rickroll mode\n");
        //set rickroll
        pthread_mutex_lock(&easteregg_mutex);
        ee_config.rickroll = 1;
        pthread_mutex_unlock(&easteregg_mutex);

        //and return to the status page
        char header[] = "HTTP/1.0 302 Found\r\n"
                          "Location: /\r\n\r\n";
        rio_writen(connfd, header, strlen(header));
    }
    else if(strncmp(path, "/set/norickroll", 10)==0)
    {
        printf("Unsetting rickroll mode\n");
        //clear rickroll
        pthread_mutex_lock(&easteregg_mutex);
        ee_config.rickroll = 0;
        pthread_mutex_unlock(&easteregg_mutex);

        //and return to the status page
        char header[] = "HTTP/1.0 302 Found\r\n"
                          "Location: /\r\n\r\n";
        rio_writen(connfd, header, strlen(header));
    }
    //other conditions here
    else
    {
        char header[] = "HTTP/1.0 200 OK\r\n"
                          "Content-Type: text/html\r\n\r\n";
        rio_writen(connfd, header, strlen(header));

        char response[] = "<h1>Easter Egg Manager</h1><hr />"
                          "<b>The settings:</b><br /><br />";
        rio_writen(connfd, response, strlen(response));



        char dynamiccontent[MAXLINE];
        memset(dynamiccontent, '\0', MAXLINE*sizeof(char));
        
        pthread_mutex_lock(&easteregg_mutex);

        snprintf(dynamiccontent, MAXLINE, 
                                "<table style='border-left: 1px black solid' >"
                                "<tr><td>NOPE Mode:</td><td>%s</td></tr>"
                                "<tr><td>Rickroll:</td><td>%s</td></tr>"
                                "</table>",
                                (ee_config.nope)?"on":"off",
                                (ee_config.rickroll)?"on":"off");

        pthread_mutex_unlock(&easteregg_mutex);
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
