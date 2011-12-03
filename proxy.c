#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "csapp.h"

void fetchRequest(int connfd);

int main (int argc, char *argv []){
	
	int listenfd, connfd, port;
    socklen_t clientlen;
	struct sockaddr_in clientaddr;
	
	if(argc != 2){
		fprintf(stderr,"Usage %s <port>\n",argv[0]);
		exit(1);
	}
	port = atoi(argv[1]);
	listenfd = Open_listenfd(port);
	while(1) {
		clientlen = sizeof(clientaddr);
		connfd = Accept(listenfd , (SA *)&clientaddr, &clientlen);
        printf("Accepted connection\n");
		fetchRequest(connfd);
		Close(connfd);
        printf("Closed connection\n");
   }
}

void fetchRequest(int connfd){
    //@TODO: something useful

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
        printf("Executing a GET request\n");
        //now let's copy out the hostname
        //@TODO: make this not suck
        char hostname[MAXLINE]; //overkill size?
        char path[MAXLINE]; 
        char httpver[MAXLINE]; 
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
            sscanf(&buffer[i+1], "%d%s %s", &port, path, httpver);
        }
        else
        {
            sscanf(&buffer[i], "%s %s", path, httpver);
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
        Rio_writen(server_fd, httpver, strlen(httpver));
        Rio_writen(server_fd, "\r\n", strlen("\r\n"));

        printf("->\t%s%s %s%s", "GET ", path, httpver, "\r\n");


        while(Rio_readlineb(&proxy_client, buffer, MAXLINE) && 
                strncmp(buffer, "\r", 1))
        {
            Rio_writen(server_fd, buffer, strlen(buffer));
            printf("->\t%s", buffer);
        }
        //finish the request
        Rio_writen(server_fd, "\r\n", strlen("\r\n"));


        //now read from the server back to the client
        while(Rio_readlineb(&server_connection, buffer, MAXLINE) != 0)
        {
            printf("<-\t%s", buffer);
            Rio_writen(connfd, buffer, strlen(buffer));
        }

        Close(server_fd);
   }
}
