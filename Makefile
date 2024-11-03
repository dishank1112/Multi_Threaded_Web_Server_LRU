CC = g++
CFLAGS = -g -Wall 

all: proxy

proxy: proxy_server_with_cache.o proxy_parse.o
	$(CC) $(CFLAGS) -o proxy proxy_server_with_cache.o proxy_parse.o

proxy_parse.o: proxy_parse.c proxy_parse.h
	$(CC) $(CFLAGS) -c proxy_parse.c

proxy_server_with_cache.o: proxy_server_with_cache.c proxy_parse.h
	$(CC) $(CFLAGS) -c proxy_server_with_cache.c

clean:
	del /Q proxy.exe *.o

tar:
	tar -cvzf ass1.tgz proxy_server_with_cache.c README Makefile proxy_parse.c proxy_parse.h
