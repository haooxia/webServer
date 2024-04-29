Server: Server.c main.c
	gcc -o Server Server.c main.c -Wall

clean:
	rm -r Server