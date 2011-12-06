#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <time.h>

#include "csapp.h"

//#define DEBUG
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

#define MAX_OBJECT_SIZE 102400 /* 100 KB */
#define MAX_CACHE_SIZE 1048576 /* 1 MB */

//cache implemented as a lined list
//@TODO: either finish implementing ArrayList cache or remove this comment
struct cachenode
{
    char* header;
    void* data;
    int size;
    struct cachenode* prev;
    struct cachenode* next;
};

struct listcache
{
    int totalsize;
    struct cachenode* head;
    struct cachenode* tail;
};
pthread_rwlock_t cachelock;


//@TODO: fix this or remove it
typedef struct {
	char *header;
	time_t stamp;
	void *data;
	size_t size;
} object;

int cacheSize;
int cacheLength;
int arrayLength;
//arraylist of object pointers
object **cache;

//for handling the connection
void handleConnection(int connfd);
void* newConnectionThread(void* arg);

//get the hostname and path from a URL
void parseURL(char buffer[MAXLINE], char* hostname, char* path, int *port);
//make a GET request to the server
//returns whether or not the headers think it's a good idea to cache
void makeGETRequest(char* hostname, 
                    char* path,
                    int port,
                    rio_t* proxy_client,
                    int server_fd);
//copy the HTTP request from the client to the server
void copyRequest(int server_fd, rio_t* proxy_client);
//read back from the server to the client
void serveToClient(int connfd, rio_t* server_connection, 
        char* hostname, char* path, int cachestatus);



//feature functions
//take over the connection and print the feature console
void featureConsole(int connfd, rio_t* proxy_client, char path[MAXLINE]);
//change the host, request, and port based on feature settings
int handleFeatures(char* hostname, char* path, int* port);

/* 
 * Cache Functions
 * //@TODO: fix this or remove it
 */
//add the object to the cache
void add_object(object *o);
//evict sufficient blocks to add an object of size size to the cache
void make_space(size_t size);
//query the cache for a header of size 
object* query_cache(char *header);
//grow the arraylist
void grow_cache();

pthread_rwlock_t lock;


//List cache functions
//add an object to the cache
void add_cache_object(struct cachenode* obj);
//find an object in the cache based on header, and update LRU
//return NULL if not found
struct cachenode* get_cache_object(char* header);
//clear the cache
void clear_cache();

//global cache variable
struct listcache thecache;


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
    //caching: disable caching for debugging or for dynamic browsing
    int cache;
};
struct features_t ft_config;;

void add_object(object *o)
{
	pthread_rwlock_wrlock(&lock);
	make_space(o->size);
	if(arrayLength == cacheLength)
		grow_cache();
	int i=0;
	for(i=0;i<arrayLength;i++){
		if(cache[i] == NULL){
			cache[i] = o;
			break;
		}
	}
	o->stamp = time(NULL);
	cacheSize += o->size;
	cacheLength++;
	pthread_rwlock_unlock(&lock);
}

void grow_cache()
{
    cache = realloc(cache,sizeof(object *)*2*arrayLength);
}

void make_space(size_t size)
{
    
	while((unsigned)cacheSize >= MAX_CACHE_SIZE - size)
	{
		int i = 0;
		int min=0;
		for(i=0;i<arrayLength;i++){
			if(cache[i] && cache[i]->stamp < cache[min]->stamp)
				min = i;
		}
		cacheLength--;
		cacheSize -= cache[min]->size;
		free(cache[min]->header);
		free(cache[min]->data);
		free(cache[min]);
		cache[min] = NULL;
	}
}

int chrcmpn(char *s1,char *s2,int n)
{
	int i;
	int sum=0;
	for(i=0;i<n;i++)
		sum+= s1[i] - s2[i];
	return sum;
}

object *query_cache(char *header)
{
	int i=0;
	object *result = NULL;
	for(i=0;i<arrayLength;i++)
	{
		if(cache[i] && 
		   !strncmp(cache[i]->header,header,strlen(cache[i]->header)))
		{
			return cache[i];
		}
	}
	return result;
}

int open_clientfd_r(char *hostname, int port) 
{
    int clientfd;
    struct hostent *hp;
    struct sockaddr_in serveraddr;
	//@TODO: Are socket and connect thread safe
    if ((clientfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		return -1; /* check errno for cause of error */
	
    /* Fill in the server's IP address and port */

    //@TODO: use gethostbyname_r

    if ((hp = gethostbyname(hostname)) == NULL)
		return -2; /* check h_errno for cause of error */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)hp->h_addr_list[0], 
		  (char *)&serveraddr.sin_addr.s_addr, hp->h_length);
    serveraddr.sin_port = htons(port);
	
    /* Establish a connection with the server */
    if (connect(clientfd, (SA *) &serveraddr, sizeof(serveraddr)) < 0)
		return -1;
    return clientfd;
}



int main (int argc, char *argv []){
	//@TODO: sigactions
	signal(SIGPIPE, SIG_IGN);
	
	int listenfd, connfd, port;
    socklen_t clientlen;
	struct sockaddr_in clientaddr;
	cache = malloc(sizeof(object *) * 10);
	cacheLength = 0;
	cacheSize = 0;
	arrayLength = 10;
	pthread_rwlock_init(&lock,NULL);
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
    ft_config.cache = 1;


    //initialize mutexes
    pthread_mutex_init(&features_mutex, NULL);
    pthread_rwlock_init(&cachelock, NULL);

    //initialize cache
    thecache.totalsize = 0;
    thecache.head = NULL;
    thecache.tail = NULL;

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
	return 1; //never gets here
}
void* newConnectionThread(void* arg)
{
    pthread_detach(pthread_self());
	int connfd = *(int*)arg;
    free(arg);
    handleConnection(connfd);
    return NULL;
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
        //debug_printf("Executing a GET request\n");

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
        //manipulate the request based on the features
        //get the cache status: 1 = dumb, 2 = smart, 0 = off
        int cachestatus = handleFeatures(hostname, path, &port);

       
        //search the cache
        if(cachestatus)
        {
            char header[strlen(hostname)+strlen(path)+1];
            sprintf(header, "%s%s", hostname, path);
            //struct cachenode* obj = get_cache_object(header);
            object* obj = query_cache(header);
            if(obj)
            {
                debug_printf("Serving object %s from the cache! (Size %u)\n",
                        header, (unsigned)obj->size);

                //@TODO: error check?
                rio_writen(connfd, obj->data, obj->size);
                free(obj->data);
                free(obj->header);
                free(obj);

                close(connfd);
                return;
            }
            else
            {
                debug_printf("Could not find %s in the cache\n", header);
            }
        }

        //some debug statements
       // debug_printf("Trying to contact hostname %s on port %d\n",
        //              hostname, port);
        //debug_printf("I'll ask him for the path '%s'\n", path);


        //open the connection to the remote server
        if((server_fd = open_clientfd_r(hostname, port)) < 0)
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
        serveToClient(connfd, &server_connection, hostname, path, 1);//cachestatus);

        //clean up
        close(server_fd);
        //debug_printf("Closed connection to %s%s\n", hostname, path);
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
        //don't send proxy-connection or cache-control headers
        if((strncmp(buffer, "Proxy-Connection: ", 18) != 0)
           && (strncmp(buffer, "Cache-Control: ", 14) != 0))
        {
            rio_writen(server_fd, buffer, strlen(buffer));
            //verbose_printf("->\t%s", buffer);
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

    //verbose_printf("->\t%s%s HTTP/1.0 \r\n", "GET ", path);

    copyRequest(server_fd, proxy_client);
    //
    //finish the request
    rio_writen(server_fd, "\r\n", strlen("\r\n"));

    //@TODO: either use these params or remove them from the sig
    if(hostname == hostname || port == port){}
}

void serveToClient(int connfd, rio_t* server_connection, 
        char* hostname, char* path, int cachestatus)
{
    int shouldcache = 0; //smart caching: do the headers say we should cache?

    //we'll build up this cache object
    

  
    char tempbuffer[MAX_OBJECT_SIZE];
    int bufferpos=0;

    char buffer[MAXLINE];

    int n = 0; //number of bytes
    while(((n=rio_readlineb(server_connection, buffer, MAXLINE)) != 0) && 
            buffer[0] != '\r')
    {
        //verbose_printf("<-\t%s", buffer);
        if(rio_writen(connfd, buffer, strlen(buffer)) < 0)
        {
            printf("Write error from %s%s\n", hostname, path);
            break;
        }
        if((bufferpos + strlen(buffer)) >= MAX_OBJECT_SIZE)
        {
            //null out the object to signify that it's too big

			debug_printf("OMG HEADER SOOOO BIG \n");
        }
		n=sprintf(tempbuffer+bufferpos, "%s", buffer);
		if(n > 0)
		{
			bufferpos+=n+1;
		}
		else
		{
			printf("\n\nError!\n\n");
		}

        //see if the header says anything about caching
        //this will do nothing if there's no match
        if(shouldcache >= 0)
        {
            sscanf(buffer, "Cache-Control: max-age=%d", &shouldcache);
        }

        //if the headers say explicitly that we shouldn't cache, then absolutely
        //don't cache.
        if(strncmp(buffer, "Cache-Control: private", 22)
           || strncmp(buffer, "Cache-Control: no-cache", 23))
        {
            shouldcache = -1;
        }
    }
    rio_writen(connfd, "\r\n", strlen("\r\n"));
    
    if((bufferpos+2) >= MAX_OBJECT_SIZE)
    {
       debug_printf("OMG HEADER SOOOO BIG \n");
    }
	
	object* cacheobj = malloc(sizeof(object));
    
	//sscanf(&tempbuffer[bufferpos], "\r\n");
	//bufferpos+=2;
	bufferpos+=sprintf(&tempbuffer[bufferpos], "\r\n" );
	char *header = calloc(sizeof(char),bufferpos);
	memcpy(header, tempbuffer, bufferpos);
	
	pthread_rwlock_rdlock(&lock);
	object *hit=query_cache(header);
	if(hit)
	{
		rio_writen(connfd,hit->data,hit->size);
		pthread_rwlock_unlock(&lock);
		pthread_rwlock_wrlock(&lock);
		hit->stamp = time(NULL);
		pthread_rwlock_unlock(&lock);
		verbose_printf("cache hit size %d\n",(int)hit->size);
		return;
	}
	pthread_rwlock_unlock(&lock);
	cacheobj->header = header;
	debug_printf("header %s",header);	
	
    //if we're absolutely not caching, then set shouldcache to false
    if(shouldcache == -1)
    {
        shouldcache = 0;
    }
    
    //@TODO: cache this
	bufferpos = 0;
	tempbuffer [0] = '\0';
    while((n=rio_readnb(server_connection, buffer, MAXLINE)) >0)
    {
        if(rio_writen(connfd, buffer, n) < 0)
        {
            printf("Error writing from %s%s\n", hostname, path);
            //error on write
            free(cacheobj->header);
			free(cacheobj);
			return;
        }
		n=sprintf(tempbuffer+bufferpos, "%s", buffer);
		bufferpos += n+1;
        //verbose_printf("<-\t%s", buffer);
        memset(buffer, '\0', MAXLINE*sizeof(char));
        //printf("\t Bufferpos is %d\n", bufferpos);
    }
	debug_printf("data %s",tempbuffer);
	char *data = calloc(sizeof(char),bufferpos);
	memcpy(data, tempbuffer, bufferpos);
	cacheobj->data = data;
	cacheobj->size = bufferpos;
	verbose_printf("size = %d\n",(int)(cacheobj->size));
	if(cacheobj->size > MAX_OBJECT_SIZE)
	{
		free(cacheobj->header);
		free(cacheobj->data);
		free(cacheobj);
	}
	
    if(cacheobj)
    {
        cacheobj->size = bufferpos;
        printf("Bufferpos %d stored as size %d\n", bufferpos, (int)cacheobj->size);
        cacheobj->data = malloc(bufferpos * sizeof(char));
        memcpy(cacheobj->data, tempbuffer, bufferpos);

        if(cachestatus == 1) //1 = cache, 2 = smart cache, 0 = don't cache
        {
            //@TODO: fix or delete arraylist cache
            //add_object(cacheobj);

            debug_printf("Added object '%s' to the cache\n",
                            cacheobj->header);
            add_object(cacheobj);
            add_object(cacheobj);
        }
        else if(cachestatus == 2)
        {
            if(shouldcache)
            {
                //@TODO: fix or delete arraylist cache
                //add_object(cacheobj)

                add_object(cacheobj);
                add_object(cacheobj);
                debug_printf("Added object '%s' to the cache\n",
                                cacheobj->header);
            }
            else
            {
                //smart caching says no
                debug_printf("Smart cache: skipping the cache\n");
                free(cacheobj->header);
                free(cacheobj->data);
                free(cacheobj);
            }
        }
        else
        {
            debug_printf("Cache disabled: skipping the cache\n");
            free(cacheobj->header);
            free(cacheobj->data);
            free(cacheobj);
        }
    }
    else
    {
        debug_printf("Object was too big for cache, didn't cache it\n");
    }
}

/***********
 ** List Cache functions
 ***********/

//add an object to the cache
void add_cache_object(struct cachenode* obj)
{
    if(obj->size > MAX_OBJECT_SIZE)
        return; //discard it
    pthread_rwlock_wrlock(&cachelock);
    int availablesize = 1024*1024 - (int)thecache.totalsize;
    availablesize -= (int)obj->size;

    while(availablesize < 0)
    {
        //while there's not enough space, knock out oldest entry
        struct cachenode* end = thecache.tail;
        if(end == NULL)
        {
            break;
        }
        struct cachenode* newend = end->prev;
        thecache.totalsize = thecache.totalsize - end->size;
        printf("Freed %d bytes from the cache\n", end->size);

        free(end->header);
        free(end->data);
        free(end);
        if(newend)
            newend->next = NULL;
        thecache.tail = newend;
        availablesize = 1024*1024 - (int)thecache.totalsize - obj->size;
    }
    printf("\n\nAvailable size: %d\n\n", availablesize);

    //@FIXME: this is like an assert
    if(availablesize < 0){exit(1);}

    //now add the new entry to the front of the list
    obj->prev = NULL;
    obj->next = thecache.head;
    if(obj->next)
        obj->next->prev = obj;
    thecache.head = obj;
    if(thecache.tail == NULL)
        thecache.tail = obj;
    thecache.totalsize += obj->size;

    debug_printf("\tNew total cache size is %u\n", thecache.totalsize);

    pthread_rwlock_unlock(&cachelock);
}

//find an object in the cache based on header, and update LRU
//return NULL if not found
struct cachenode* get_cache_object(char* header)
{
    //@TODO: actually use readlocking
    //right now, we just use write locking for LRU
    pthread_rwlock_wrlock(&cachelock);
    struct cachenode* obj = thecache.head;
    while(obj)
    {
        if(strcmp(obj->header, header) == 0)
        {
            //found cache object
            //move it to the head of the list
            struct cachenode* prev = obj->prev;
            struct cachenode* next = obj->next;
            if(prev)
                prev->next = next;
            if(next)
                next->prev = prev;
            obj->prev = NULL;
            obj->next = thecache.head;
            if(obj->next == obj)
            {
                obj->next = NULL;
            }
            if(obj->next)
                obj->next->prev = obj;
            thecache.head = obj;

            //now we allocate a new cache object, copy the entry into it, and
            //return that instead. We do this in case the cache entry gets freed
            //after return but before usage
            //
            //it is the caller's responsibility to free it
            struct cachenode* ret = malloc(sizeof(struct cachenode));
            ret->data = malloc(obj->size);
            memcpy(ret->data, obj->data, obj->size);
            ret->size = obj->size;
            ret->header = NULL; //we don't care about the header, and free(NULL)
                                //                                 does nothing.
            pthread_rwlock_unlock(&cachelock);
            return ret;
        }
        obj = obj->next;
    }
    pthread_rwlock_unlock(&cachelock);
    return NULL;
}


//clear the cache
void clear_cache()
{
    pthread_rwlock_wrlock(&cachelock);
    struct cachenode* n = thecache.head;
    while(n)
    {
        struct cachenode* next = n->next;
        free(n->header);
        free(n->data);
        free(n);
        n = next;
    }
    thecache.head = NULL;
    thecache.tail = NULL;
    thecache.totalsize = 0;
    pthread_rwlock_unlock(&cachelock);
}

/*************
 ** Feature Functions
 ** Not related to the core functionality of the proxy
 *************/
//returns whether or not caching is enabled
int handleFeatures(char* hostname, char* path, int* port)
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
        size_t n = sprintf(path, "/5258/5582667252_b3b46db1ec_b.jpg");
        path[n]='\0'; //null-terminate ALL THE THINGS
        *port=80;
    }
    if(features.rickroll)
    {
        //redirect any youtube link to rickroll
        if( ((strcmp(hostname, "youtube.com") == 0)
            || (strcmp(hostname, "www.youtube.com") == 0))
           && (strncmp(path, "/watch", 6) == 0))
        {
            size_t n = sprintf(path, "/watch?v=oHg5SJYRHA0");
            path[n]='\0'; //null-terminate ALL THE THINGS
        }
    }
    return features.cache;
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
    else if(strncmp(path, "/set/rickroll", 13)==0)
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
    else if(strncmp(path, "/set/norickroll", 15)==0)
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
    else if(strncmp(path, "/set/cache/dumb", 15)==0)
    {
        printf("Setting dumb cache\n");
        //set cache
        pthread_mutex_lock(&features_mutex);
        ft_config.cache = 1;
        pthread_mutex_unlock(&features_mutex);

        //and return to the status page
        char header[] = "HTTP/1.0 302 Found\r\n"
                          "Location: /\r\n\r\n";
        rio_writen(connfd, header, strlen(header));
    }
    else if(strncmp(path, "/set/cache/smart", 16)==0)
    {
        printf("Setting smart cache\n");
        //set cache
        pthread_mutex_lock(&features_mutex);
        ft_config.cache = 2;
        pthread_mutex_unlock(&features_mutex);

        //and return to the status page
        char header[] = "HTTP/1.0 302 Found\r\n"
                          "Location: /\r\n\r\n";
        rio_writen(connfd, header, strlen(header));
    }
    else if(strncmp(path, "/set/nocache", 12)==0)
    {
        printf("Setting cache off\n");
        //clear cache mode
        pthread_mutex_lock(&features_mutex);
        ft_config.cache = 0;
        pthread_mutex_unlock(&features_mutex);

        clear_cache();

        //and return to the status page
        char header[] = "HTTP/1.0 302 Found\r\n"
                          "Location: /\r\n\r\n";
        rio_writen(connfd, header, strlen(header));
    }
    else if(strncmp(path, "/clearcache", 11)==0)
    {
        printf("Clearing cache\n");
        //clear the cache
        clear_cache();

        //and return to the status page
        char header[] = "HTTP/1.0 302 Found\r\n"
                          "Location: /info\r\n\r\n";
        rio_writen(connfd, header, strlen(header));
    }
    else if(strncmp(path, "/info", 5)==0)
    {
        char header[] = "HTTP/1.0 200 OK\r\n"
                          "Content-Type: text/html\r\n\r\n";
        rio_writen(connfd, header, strlen(header));

        char response[] = "<title>Proxy Diagnostic Page</title>"
                          "<h1>Cache Diagnostics</h1><hr />"
                          "<a href='/clearcache'>Clear the Cache</a>"
                          "<br />"
                          "<a href='/'>Back</a><br />";
        rio_writen(connfd, response, strlen(response));
        
        char data[MAXLINE];
        int n = 0;
        
        //let's read the cache
        pthread_rwlock_rdlock(&cachelock);
        double percentfull = ((double)thecache.totalsize*100.0);
        percentfull /= (double)MAX_CACHE_SIZE;

        n = sprintf(data, "Total cache size is <b>%u bytes (%.2f%%)</b>"
                      "<br /><br />"
                      "<style>"
                      "table{table-layout: fixed;}"
                      "td{width: 45%%;}"
                      "</style>"
                      "Here's what's in the cache (in order):<br />"
                      "<table><tr><th>Size</th>"
                      "<th>Header</th></tr>",
                      thecache.totalsize, percentfull);
        rio_writen(connfd, data, n);

        struct cachenode* node = thecache.head;
        while(node)
        {
            n=sprintf(data, "<tr>"
                            "<td>%u bytes</td><td>%s</td>"
                            "</tr>", 
                                node->size, node->header);
            rio_writen(connfd, data, n);
            node = node->next;
        }
        n=sprintf(data, "</table>");
        rio_writen(connfd, data, n);

        pthread_rwlock_unlock(&cachelock);
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
                                "<tr><td>Caching Mode:</td><td>%s</td></tr>"
                                "<tr><td>NOPE Mode:</td><td>%s</td></tr>"
                                "<tr><td>Rickroll:</td><td>%s</td></tr>"
                                "</table>",
                                (ft_config.cache)?
                                  ((ft_config.cache == 2)?"smart":"dumb"):"off",
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
                         "  <td><a href='/set/cache/dumb'>"
                         "      Engage Dumb Caching"
                         "  </a></td>"
                         "  <td><a href='/set/cache/smart'>"
                         "      Engage Smart Caching"
                         "  </a></td>"
                         "</tr>"
                         "<tr>"
                         "  <td><a href='/set/nocache'>"
                         "      Disable Caching"
                         "  </a></td>"
                         "  <td><a href='/info'>"
                         "      Cache Diagnostics"
                         "  </a></td>"
                         "</tr>"
                         "<tr>"
                         "<td style='background-color:black' colspan='2'>"
                         "</tr>"
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
                         "</table>";
        rio_writen(connfd, options, strlen(options));

        char response2[] = "<br /><hr /><i>This proxy server written by Jeff Cooper and "
                   "Prashant Sridhar, and powered by Caffeine.</i>";
        rio_writen(connfd, response2, strlen(response2));
    }

    close(connfd);
}
