#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>

/******************
 ** NOTE: using the thread-friendly version of csapp.*
 ** This is a version that we modified to use pthread_exit() instead of exit()
 ** when it dies, allowing us to take advantage of the capital-letter error
 ** handling functions (which have been renamed to tt_Rio_*).
 **
 **/
#include "csapp.h"

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
#define MAX_CACHE_SIZE 1048576 /* 1 MB */

//cache implemented as a lined list
struct cachenode
{
    char* header;
    void* data;
    char* objname;
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



//for handling the connection
void handle_connection(int connfd);
void* new_connection_thread(void* arg);

//get the hostname and path from a URL
void parse_url(char buffer[MAXLINE], char* hostname, char* path, int *port);
//make a GET request to the server
//returns whether or not the headers think it's a good idea to cache
void make_GET_request(char* path,
                    char* buffer,
                    int server_fd);
//copy the HTTP request from the client to a buffer
char* copy_request(rio_t* proxy_client);
//write a buffer to the server
void write_request(int server_fd, char* buffer);
//read back from the server to the client
void serve_to_client(int connfd, rio_t* server_connection, 
        char* hostname, char* path, int cachestatus, char* cachereq);



//feature functions
//take over the connection and print the feature console
void feature_console(int connfd, rio_t* proxy_client, char path[MAXLINE]);
//change the host, request, and port based on feature settings
int handle_features(char* hostname, char* path, int* port);

//List cache functions
//add an object to the cache
void add_cache_object(struct cachenode* obj);
//find an object in the cache based on header, and update LRU
//return NULL if not found
struct cachenode* get_cache_object(char* objname, char* header);
//clear the cache
void clear_cache();
//cache unlock handler: if a thread dies, unlock the cache
void unlock_cache_handler(void* ptr);

//new cachenode
struct cachenode* newNode();
//free node
void free_node(struct cachenode* n);

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


int open_clientfd_r(char *hostname, int port) 
{
    int clientfd;
    struct hostent *hp;
	struct hostent ret;
    struct sockaddr_in serveraddr;
	if ((clientfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		return -1; /* check errno for cause of error */
	
    /* Fill in the server's IP address and port */

    char buffer[MAXLINE];
	int errno;
	
    if (gethostbyname_r(hostname,&ret,buffer,MAXLINE,&hp,&errno) != 0)
		return errno; 
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    if(!hp)
        return -1;
    bcopy((char *)hp->h_addr_list[0], 
		  (char *)&serveraddr.sin_addr.s_addr, hp->h_length);
    serveraddr.sin_port = htons(port);
	
    /* Establish a connection with the server */
    if (connect(clientfd, (SA *) &serveraddr, sizeof(serveraddr)) < 0)
		return -1;
    return clientfd;
}




int main (int argc, char *argv []){
	signal(SIGPIPE, SIG_IGN);
	
	int listenfd, connfd, port;
    socklen_t clientlen;
	struct sockaddr_in clientaddr;
	if(argc != 2){
		fprintf(stderr,"Usage %s <port>\n",argv[0]);
		exit(1);
	}
	port = atoi(argv[1]);
	printf("Proxy Started!\n==========================\n");
    printf("\tRunning on port %d\n\tRunning in %s\n"
            "\tBrowse to http://proxy-configurator/ "
            "for info and options\n\nBy Jeff Cooper and Prashant Sridhar\n",
            port,
            #ifdef SEQUENTIAL
            "sequenial mode"
            #else
            "parallel mode"
            #endif
            );

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
        handle_connection(connfd);
#else
        int* fd_place = malloc(sizeof(int));
        *fd_place = connfd;
        pthread_create(&tid,
                       NULL,
                       new_connection_thread,
                       (void*)fd_place);
#endif
    }
	return 1; //never gets here
}
void* new_connection_thread(void* arg)
{
    pthread_detach(pthread_self());
	int connfd = *(int*)arg;
    free(arg);
    handle_connection(connfd);
    return NULL;
}

void handle_connection(int connfd){
    char buffer[MAXLINE];
    memset(buffer, '\0', MAXLINE*sizeof(char));
    rio_t proxy_client;
    t_Rio_readinitb(&proxy_client, connfd);

    int server_fd;
    rio_t server_connection;

    //get the first line of the request into the buffer
    t_Rio_readlineb(&proxy_client, buffer, MAXLINE);
    if(strncmp(buffer, "GET http:", 9) == 0)
    {
        //we've got a get request
        //debug_printf("Executing a GET request\n");

        //parse the URL (hostname, path, and port) from the first line
        char hostname[MAXLINE];
        char path[MAXLINE];
        int port=80;
        parse_url(buffer, hostname, path, &port);

        //if we're trying to access the features console
        //  then call the feature handler and end the function
        if((strcmp(hostname, "proxy-configurator") == 0))
        {
            //manage the features
            feature_console(connfd, &proxy_client, path);
            //and done
            return;
        }
        //manipulate the request based on the features
        //get the cache status: 1 = dumb, 2 = smart, 0 = off
        int cachestatus = handle_features(hostname, path, &port);

        char* requestheader = copy_request(&proxy_client);

       
        //search the cache
        if(cachestatus)
        {
            char name[strlen(hostname)+strlen(path)+1];
            sprintf(name, "%s%s", hostname, path);

            struct cachenode* obj = get_cache_object(name, requestheader);
            if(obj)
            {
                debug_printf("Serving object %s from the cache! (Size %u)\n",
                        path, (unsigned)obj->size);

                
                t_Rio_writen(connfd, obj->data, obj->size);
                free_node(obj);

                close(connfd);
                return;
            }
            else
            {
                debug_printf("Could not find %s in the cache\n", path);
            }
        }

        //open the connection to the remote server
		//pthread_rwlock_rdlock(&cachelock);
		if((server_fd = open_clientfd_r(hostname, port)) < 0)
        {
            char errorbuf[] = "HTTP 404 NOTFOUND\r\n\r\n404 Not Found\r\n";
            t_Rio_writen(connfd, errorbuf, strlen(errorbuf));
            close(connfd);
            return;
        }
		//pthread_rwlock_rdlock(&cachelock);
        t_Rio_readinitb(&server_connection, server_fd);

        //now, make the GET request to the server
        make_GET_request(path,requestheader, server_fd);

        
        //now read from the server back to the client
        serve_to_client(connfd, &server_connection, hostname, 
            path, cachestatus, requestheader);

        //clean up
        close(server_fd);
        //debug_printf("Closed connection to %s%s\n", hostname, path);
    }
    else
    {
        //if we don't have a GET request with http, throw an error
        char errorbuf[] = "HTTP 500 ERROR\r\n\r\n";
        t_Rio_writen(connfd, errorbuf, strlen(errorbuf));
    }
    close(connfd);
}

//parse a URL and set the hostname and path into the given buffers
void parse_url(char buffer[MAXLINE], char* hostname, char* path, int *port)
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

//copy request headers from the client into a buffer
//guaranteed to be complete lines
char* copy_request(rio_t* proxy_client)
{
    int currentsize = MAX_OBJECT_SIZE;
    char* tempbuffer = malloc(sizeof(char)*currentsize);
    int bufferpos = 0;
    ssize_t n;
    char buffer[MAXLINE];
    while((n=t_Rio_readlineb(proxy_client, buffer, MAXLINE)) && 
            strncmp(buffer, "\r", 1))
    {
        //don't send proxy-connection or cache-control headers
        if((strncmp(buffer, "Proxy-Connection: ", 18) != 0)
           && (strncmp(buffer, "Cache-Control: ", 14) != 0))
        {
            if((bufferpos + n) > currentsize-1)
            {
                currentsize *= 2;
                tempbuffer = realloc(tempbuffer, currentsize);
            }
            bufferpos+=sprintf(&tempbuffer[bufferpos], "%s", buffer);
        }
    }
    tempbuffer[bufferpos] = '\0';
    char* requestheaders = calloc(bufferpos+1, sizeof(char));
    memcpy(requestheaders, tempbuffer, bufferpos+1);
    free(tempbuffer);
    return requestheaders;
}

void write_request(int server_fd, char* buffer)
{
    t_Rio_writen(server_fd, buffer, strlen(buffer));
   	t_Rio_writen(server_fd, "\r\n", 2);
}
void make_GET_request(char* path,
                    char* buffer,
                    int server_fd)
{
    //make the GET request
    t_Rio_writen(server_fd, "GET ", strlen("GET "));
    t_Rio_writen(server_fd, path, strlen(path));
    t_Rio_writen(server_fd, " ", strlen(" "));
    t_Rio_writen(server_fd, "HTTP/1.0", strlen("HTTP/1.0"));
    t_Rio_writen(server_fd, "\r\n", strlen("\r\n"));

    verbose_printf("->\t%s%s HTTP/1.0 \r\n", "GET ", path);

    write_request(server_fd, buffer);

}

void serve_to_client(int connfd, rio_t* server_connection, 
        char* hostname, char* path, int cachestatus, char* cachereq)
{
    int shouldcache = 0; //smart caching: do the headers say we should cache?

    //we'll build up this cache object
    struct cachenode* cacheobj = newNode();
    cacheobj->objname = calloc(strlen(hostname)+strlen(path)+1, sizeof(char));
    sprintf(cacheobj->objname, "%s%s", hostname, path);
	cacheobj->header = cachereq;
    

  
    char tempbuffer[MAX_OBJECT_SIZE];
    int bufferpos=0;

    char buffer[MAXLINE];
    memset(buffer, '\0', MAXLINE);

    int n = 0; //number of bytes
    while(((n=t_Rio_readlineb(server_connection, buffer, MAXLINE)) != 0) && 
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
			bufferpos+=n;
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
    t_Rio_writen(connfd, "\r\n", strlen("\r\n"));
    
    if((bufferpos+2) >= MAX_OBJECT_SIZE)
    {
       debug_printf("OMG HEADER SOOOO BIG \n");
    }
    
    tempbuffer[bufferpos++] = '\r';
    tempbuffer[bufferpos++] = '\n';
   
	
	
    //if we're absolutely not caching, then set shouldcache to false
    if(shouldcache == -1)
    {
        shouldcache = 0;
    }
    
    while((n=t_Rio_readnb(server_connection, buffer, MAXLINE)) >0)
    {
        if(rio_writen(connfd, buffer, n) < 0)
        {
			printf("Error writing from %s%s\n", hostname, path);
            //error on write
            free_node(cacheobj);
			return;
        }
		//n=sprintf(tempbuffer+bufferpos, "%s", buffer);
        if(bufferpos+n < MAX_OBJECT_SIZE)
        {
            memcpy(tempbuffer+bufferpos, buffer, n);
        }
        bufferpos += n;
        //verbose_printf("<-\t%s", buffer);

        memset(buffer, '\0', MAXLINE*sizeof(char));
    }
    
	if(bufferpos < MAX_OBJECT_SIZE)
    {
        char *data = calloc(sizeof(char),bufferpos);
    	memcpy(data, tempbuffer, bufferpos);
        cacheobj->data = data;
    }
	cacheobj->size = bufferpos;
	debug_printf("size = %d\n",(int)(cacheobj->size));
	if(cacheobj->size >= MAX_OBJECT_SIZE)
	{
        free_node(cacheobj);
        cacheobj = NULL;
	}
	
    if(cacheobj)
    {
        cacheobj->size = bufferpos;
        cacheobj->data = calloc(bufferpos, sizeof(char));
        memcpy(cacheobj->data, tempbuffer, bufferpos);

        if(cachestatus == 1) //1 = cache, 2 = smart cache, 0 = don't cache
        {
            

            debug_printf("Added object '%s%s' to the cache\n",
                            hostname, path);
            add_cache_object(cacheobj);
        }
        else if(cachestatus == 2)
        {
            if(shouldcache)
            {
                
                debug_printf("Added object '%s' to the cache\n",
                                cacheobj->header);
                add_cache_object(cacheobj);
            }
            else
            {
                //smart caching says no
                debug_printf("Smart cache: skipping the cache\n");
                free_node(cacheobj);
            }
        }
        else
        {
            debug_printf("Cache disabled: skipping the cache\n");
            free_node(cacheobj);
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
    {
        free_node(obj);
        printf("Discarded object: too big\n");
        return; //discard it
    }
    debug_printf("Write locking the cache to add an object\n");
    pthread_rwlock_wrlock(&cachelock);

    //now add the new entry to the front of the list
    obj->prev = NULL;
    obj->next = thecache.head;
    if(obj->next)
        obj->next->prev = obj;
    thecache.head = obj;
    if(thecache.tail == NULL)
        thecache.tail = obj;
    thecache.totalsize += obj->size;


    while(thecache.totalsize > MAX_CACHE_SIZE)
    {
        //while there's not enough space, knock out oldest entry
        struct cachenode* end = thecache.tail;
        if(end == NULL)
        {
            thecache.totalsize = 0;
            thecache.head = NULL;
            break;
        }
        struct cachenode* newend = end->prev;
        thecache.totalsize = thecache.totalsize - end->size;
        debug_printf("Freed %d bytes from the cache\n", end->size);

        free_node(end);
        if(newend)
            newend->next = NULL;
        thecache.tail = newend;
        //if we've freed the whole cache, update the head pointer appropriately 
        if(thecache.tail == NULL)
        {
            thecache.head = NULL;
            thecache.totalsize = 0;
        }
    }

    debug_printf("\tNew total cache size is %u\n", thecache.totalsize);

    debug_printf("Unlocking the cache from writing\n");
    pthread_rwlock_unlock(&cachelock);
}


void update_node(struct cachenode *which)
{
    debug_printf("Locking the cache to update LRU\n");
    pthread_rwlock_wrlock(&cachelock);
    //find the object in the cache.  If it has since been removed by a
    //concurrent process, give up.
    struct cachenode* obj = thecache.head;
    
    while(obj && obj != which)
    {
        obj = obj->next;
    }

    if(!obj)
    {
        //we didn't find it, unlock the cache and give up.
        debug_printf("Unlocked the cache from LRU update (unsuccessful)\n");
        pthread_rwlock_unlock(&cachelock);
        return;
    }

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
    pthread_rwlock_unlock(&cachelock);
    debug_printf("Unlocked the cache from LRU update\n");
}

//find an object in the cache based on header, and update LRU
//return NULL if not found
struct cachenode* get_cache_object(char* hostpath, char* header)
{
    debug_printf("Read-locking the cache to search it\n");
    pthread_rwlock_rdlock(&cachelock);
    debug_printf("\tGot lock\n");
    struct cachenode* obj = thecache.head;
    while(obj)
    {
        if(strcmp(obj->objname, hostpath) == 0 
           && strcmp(obj->header, header) == 0)
        {
            //found cache object
            //now we allocate a new cache object, copy the entry into it, and
            //return that instead. We do this in case the cache entry gets freed
            //after return but before usage
            //
            //it is the caller's responsibility to free it
            
            struct cachenode* ret = newNode();
            ret->data = malloc(obj->size);
            memcpy(ret->data, obj->data, obj->size);
            ret->size = obj->size;
            ret->header = NULL; //we don't care about the header, and free(NULL)
                                //                                 does nothing.
            debug_printf("Unlocking the cache to re-lock for update\n");
            pthread_rwlock_unlock(&cachelock);

			update_node(obj);
            return ret;
        }
        obj = obj->next;
    }
    debug_printf("Unlocking the cache from search\n");
    pthread_rwlock_unlock(&cachelock);
    return NULL;
}


//clear the cache
void clear_cache()
{
    debug_printf("Locking the cache for clear\n");
    pthread_rwlock_wrlock(&cachelock);
    struct cachenode* n = thecache.head;
    while(n)
    {
        struct cachenode* next = n->next;
        free_node(n);
        n = next;
    }
    thecache.head = NULL;
    thecache.tail = NULL;
    thecache.totalsize = 0;
    debug_printf("Unlocking the cache from clear\n");
    pthread_rwlock_unlock(&cachelock);
}

//new cachenode
struct cachenode* newNode()
{
    struct cachenode* n = malloc(sizeof(struct cachenode));
    n->objname=NULL;
    n->size = 0;
    n->header = NULL;
    n->data = NULL;
    return n;
}
//free node
void free_node(struct cachenode* n)
{
    free(n->header);
    free(n->objname);
    free(n->data);
    free(n);
}

//handler function to unlock the cache if a thread dies
void unlock_cache_handler(void* ptr)
{
    pthread_rwlock_unlock((pthread_rwlock_t*)ptr);
}

/*************
 ** Feature Functions
 ** Not related to the core functionality of the proxy
 *************/
//returns whether or not caching is enabled
int handle_features(char* hostname, char* path, int* port)
{
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
void feature_console(int connfd, rio_t* proxy_client, char path[MAXLINE])
{
    //first, get all the headers in the client's request.
    //we can discard most or all of them
    char buffer[MAXLINE];
    while(t_Rio_readlineb(proxy_client, buffer, MAXLINE) && 
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
        t_Rio_writen(connfd, header, strlen(header));
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
        t_Rio_writen(connfd, header, strlen(header));
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
        t_Rio_writen(connfd, header, strlen(header));
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
        t_Rio_writen(connfd, header, strlen(header));
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
        t_Rio_writen(connfd, header, strlen(header));
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
        t_Rio_writen(connfd, header, strlen(header));
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
        t_Rio_writen(connfd, header, strlen(header));
    }
    else if(strncmp(path, "/clearcache", 11)==0)
    {
        printf("Clearing cache\n");
        //clear the cache
        clear_cache();

        //and return to the status page
        char header[] = "HTTP/1.0 302 Found\r\n"
                          "Location: /info\r\n\r\n";
        t_Rio_writen(connfd, header, strlen(header));
    }
    else if(strncmp(path, "/info", 5)==0)
    {
        char header[] = "HTTP/1.0 200 OK\r\n"
                          "Content-Type: text/html\r\n\r\n";
        t_Rio_writen(connfd, header, strlen(header));

        char response[] = "<title>Proxy Diagnostic Page</title>"
                          "<h1>Cache Diagnostics</h1><hr />";
        t_Rio_writen(connfd, response, strlen(response));
        
        char data[MAXLINE];
        int n = 0;
        
        //let's read the cache
        pthread_rwlock_rdlock(&cachelock);
        double percentfull = ((double)thecache.totalsize*100.0);
        percentfull /= (double)MAX_CACHE_SIZE;

        n = sprintf(data,
                      "<div "
                      "style='width:200px;border:1px black solid;height:30px;'>"
                      "<div "
                      "style='width:%dpx;background-color:red;height:30px;'>"
                      "</div></div>"
                      "Total cache size is <b>%u bytes (%.2f%%)</b>"
                      "<style>"
                      "table{table-layout: fixed;}"
                      "td{width: 45%%;}"
                      "</style>", 
                      2*(int)percentfull, thecache.totalsize, percentfull);
        t_Rio_writen(connfd, data, n);

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
                         "  <td><a href='/'>"
                         "      Main Control Panel"
                         "  </a></td>"
                         "</tr>"
                         "<tr>"
                         "  <td colspan='2'><center><a href='/clearcache'>"
                         "      Clear the Cache"
                         "  </a></center></td>"
                         "</tr>"
                         "</table>"
                         "<br /><br />"
                         "Here's what's in the cache (in order):<br />"
                         "<table><tr><th>Size</th>"
                         "<th>Object (headers hidden)</th></tr>";
        t_Rio_writen(connfd, options, strlen(options));

        //if the thread dies on read (pthread_exit), have it unlock the cache
        pthread_cleanup_push(unlock_cache_handler, (void*)&cachelock);


        struct cachenode* node = thecache.head;
        while(node)
        {
            n=sprintf(data, "<tr>"
                            "<td>%u bytes</td><td>%s</td>"
                            "</tr>", 
                                node->size, node->objname);
            t_Rio_writen(connfd, data, n);
            node = node->next;
        }
        n=sprintf(data, "</table>");
        t_Rio_writen(connfd, data, n);
        
        //we don't want to double-unlock the cache, so pop off the cleanup
        //handler
        pthread_cleanup_pop(0);

        pthread_rwlock_unlock(&cachelock);
    }
    //other conditions here
    else
    {
        char header[] = "HTTP/1.0 200 OK\r\n"
                          "Content-Type: text/html\r\n\r\n";
        t_Rio_writen(connfd, header, strlen(header));

        char response[] = "<title>Features Manager</title>"
                          "<h1>Features Manager</h1><hr />"
                          "<b>The settings:</b><br /><br />";
        t_Rio_writen(connfd, response, strlen(response));



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
        t_Rio_writen(connfd, dynamiccontent, strlen(dynamiccontent));

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
        t_Rio_writen(connfd, options, strlen(options));

        char response2[] = "<br /><hr /><i>This proxy server written by Jeff Cooper and "
                   "Prashant Sridhar, and powered by Caffeine.</i>";
        t_Rio_writen(connfd, response2, strlen(response2));
    }

    close(connfd);
}
