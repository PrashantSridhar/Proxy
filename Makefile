CC = gcc
CFLAGS = -g -Wall -W
LDFLAGS = -lpthread

all: proxy

csapp_threads.o: csapp_threads.c csapp_threads.h
	$(CC) $(CFLAGS) -c csapp_threads.c

proxy.o: proxy.c csapp_threads.h
	$(CC) $(CFLAGS) -c proxy.c

proxy: proxy.o csapp_threads.o

submit:
	(make clean; cd ..; tar czvf proxylab.tar.gz proxylab-handout)

clean:
	rm -f *~ *.o proxy core

