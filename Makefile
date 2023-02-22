#! /bin/bash

server:
	gcc -o server server.c
	gcc -o subscriber client_tcp.c

subscriber:
	./subscriber

clean:
	rm -r server subscriber
