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
   }
}

void fetchRequest(int connfd){
    //@TODO: something useful

    char buffer[MAXLINE];
    rio_t rio;
    Rio_readinitb(&rio, connfd);
    while(Rio_readlineb(&rio, buffer, MAXLINE))
    {
        printf("Server got:\n\t%s", buffer);
        if(buffer[0] == EOF)
        {
            return;
        }
    }
}
