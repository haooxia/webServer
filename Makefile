Server: Server.c main.c
	gcc -o Server Server.c main.c -l pthread -Wall

clean:
	rm -r Server
