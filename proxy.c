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

#define MAXTHREADS 100 /*max number of worker threads*/

void handleConnection(int connfd);
void* newConnectionThread(void* arg);

//@TODO: some way to deal with worst-case thread pileups
//       I have some ideas

//for now, just have some maximum number of threads that we can have
pthread_t threads[MAXTHREADS];

int main (int argc, char *argv []){
	int listenfd, connfd, port;
    socklen_t clientlen;
	struct sockaddr_in clientaddr;
	
	if(argc != 2){
		fprintf(stderr,"Usage %s <port>\n",argv[0]);
		exit(1);
	}
    //initialize the thread array
    for(int i=0;i<MAXTHREADS;i++)
    {
        threads[i] = (pthread_t)0;
    }
	port = atoi(argv[1]);
	listenfd = Open_listenfd(port);
	while(1) {

        //here, we loop through the array of threads, clean it out
        //by joining dead threads, and get the index for the new thread
        //O(n) on the max number of threads
        //if there's no thread available, loop until there is
        int thread_idx = -1;

        while(thread_idx == -1)
        {
            for(int i=0;i<MAXTHREADS;i++)
            {
                if(threads[i] != 0 && pthread_tryjoin_np(threads[i], NULL) == 0)
                {
                   threads[i] = (pthread_t)0;
                   thread_idx = i;
                }
                else if(threads[i] == 0)
                {
                    thread_idx = i;
                }
            }
        }

		clientlen = sizeof(clientaddr);
		connfd = Accept(listenfd , (SA *)&clientaddr, &clientlen);
        printf("Accepted connection (thread %d)\n", thread_idx);

        /*@TODO: fix multithreading... this should work, but it just doesn't
        int* fd_place = malloc(sizeof(int));
        *fd_place = connfd;
        pthread_create(&threads[thread_idx],
                       NULL,
                       newConnectionThread,
                       (void*)fd_place);
        */

        handleConnection(connfd);
   }
}
void* newConnectionThread(void* arg)
{
    int connfd = *(int*)arg;
    free(arg);
    handleConnection(connfd);
}

void handleConnection(int connfd){
    char buffer[MAXLINE];
    rio_t proxy_client;
    Rio_readinitb(&proxy_client, connfd);

    int server_fd;
    rio_t server_connection;
    //@TODO: use regexes to actually get the hostname, instead of guessing.

    //get the first line of the request into the buffer
    Rio_readlineb(&proxy_client, buffer, MAXLINE);
    if(strncmp(buffer, "GET", 3) == 0)
    {
        //we've got a get request
        debug_printf("Executing a GET request\n");
        //now let's copy out the hostname
        //@TODO: make this not suck
        char hostname[MAXLINE]; //overkill size?
        char path[MAXLINE]; 
        int port = 80;
        int i = 11; //first character after "GET http://"
        while(buffer[i] &&
                (buffer[i] != '/') &&
                (buffer[i] != ':')) //@TODO: seriously, regex.
        {
            hostname[i-11] = buffer[i];
            i++;
        }
        if(buffer[i] == ':')
        {
            sscanf(&buffer[i+1], "%d%s", &port, path);
        }
        hostname[i] = '\0';
        printf("Trying to contact hostname %s on port %d\n", hostname, port);
        printf("I'll ask him for the path '%s'\n", path);


        server_fd = Open_clientfd(hostname, port);
        Rio_readinitb(&server_connection, server_fd);

        //make the GET request
        Rio_writen(server_fd, "GET ", strlen("GET "));
        Rio_writen(server_fd, path, strlen(path));
        Rio_writen(server_fd, " ", strlen(" "));
        Rio_writen(server_fd, "HTTP/1.0", strlen("HTTP/1.0"));
        Rio_writen(server_fd, "\r\n", strlen("\r\n"));

        debug_printf("->\t%s%s HTTP/1.0 \r\n", "GET ", path);


        while(Rio_readlineb(&proxy_client, buffer, MAXLINE) && 
                strncmp(buffer, "\r", 1))
        {
            Rio_writen(server_fd, buffer, strlen(buffer));
            debug_printf("->\t%s", buffer);
        }
        //finish the request
        Rio_writen(server_fd, "\r\n", strlen("\r\n"));


        //now read from the server back to the client
        int content_length = -1;
        int chunked_encoding = 0;
        while(Rio_readlineb(&server_connection, buffer, MAXLINE) != 0 && 
                buffer[0] != '\r')
        {
            
            debug_printf("<-\t%s", buffer);
            Rio_writen(connfd, buffer, strlen(buffer));

            
            //these calls will do nothing on failure
            if(strncmp(buffer, "Transfer-Encoding: chunked", 26) == 0)
            {
                chunked_encoding = 1;
            }
            else
            {
                sscanf(buffer, "Content-Length: %d", &content_length);
            }
        }
        Rio_writen(connfd, "\r\n", strlen("\r\n"));
        
        //@TODO: cache this
        //@TODO: decide if any of this is necessary. It was before we forced
        //       HTTP/1.0, but now perhaps not so much. Still good for caching,
        //       perhaps.
        if(chunked_encoding)
        {
            printf("Doing chunked encoding\n");
            size_t chunksize;
            Rio_readlineb(&server_connection, buffer, MAXLINE);
            while(sscanf(buffer, "%x", &chunksize) && chunksize > 0)
            {
                char content[chunksize];
                Rio_writen(connfd, buffer, strlen(buffer));
                Rio_readnb(&server_connection, content, chunksize);
                Rio_writen(connfd, content, chunksize);
                debug_printf("%s", content);
                Rio_readlineb(&server_connection, buffer, MAXLINE);
            }

        }
        else if(content_length > -1)
        {
            char content[content_length];
            debug_printf("Trying to read %d bytes\n", content_length);
            Rio_readnb(&server_connection, content, content_length);
            Rio_writen(connfd, content, content_length);
        }
        else
        {
            //no content-length
            printf("No content length and no chunks\n");
            while(Rio_readlineb(&server_connection, buffer, MAXLINE) != 0)
            {
                Rio_writen(connfd, buffer, strlen(buffer));
                debug_printf("<-\t%s", buffer);
            }
            Rio_writen(connfd, "\r\n", strlen("\r\n"));
        }

        Close(server_fd);
		Close(connfd);
        printf("Closed connection\n");
   }
}
