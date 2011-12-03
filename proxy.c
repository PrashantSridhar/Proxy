#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "csapp.h"

void fetchRequest(int connfd);

int main (int argc, char *argv []){
	
	int listenfd, connfd, port, clientlen;
	struct sockaddr_in clientaddr;
	
	if(argc != 2){
		fprintf(stderr,"ussage %s <port>\n",argv[0]);
		exit(1);
	}
	port = atoi(argv[1]);
	listenfd = Open_listenfd(port);
	while(1) {
		clientlen = sizeof(clientaddr);
		connfd = Accept(listenfd , (SA *)&clientaddr, &clientlen);
		fetchRequest(connfd);
		Close(connfd);
}
	
void fetchRequest(int connfd){
	size_t n;
	char buf
}

