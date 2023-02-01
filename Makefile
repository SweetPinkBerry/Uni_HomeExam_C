all: client server

client:
	gcc -g -std=gnu11 client.c send_packet.c -o client

server:
	gcc -g -std=gnu11 server.c send_packet.c -o server

clean:
	rm client server

#make commands used for testing
runclient:
	valgrind ./client 127.0.0.1 24001 0.5

runserver:
	valgrind ./server 24001 NOTES.txt 3 0.5

cleanall:
	rm client server *kernel-file*
