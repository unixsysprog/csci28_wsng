#
#
# makefile for webserver
#

CC = gcc -Wall

wsng: wsng.o socklib.o web-time.o varlib.o
	$(CC) -o wsng wsng.o socklib.o web-time.o varlib.o

clean:
	rm -f *.o core wsng
