CC = gcc 
CFLAGS = -g -Wall 
LIBDIR = ./mylib
SRCDIR = ./src
INCDIR = ./include
TESTDIR = ./testfile
NAMEPROG = supermarketProject.c
LIBLINK = -Wl,-rpath,./mylib -L ./mylib -lmyQueue -lpthread

.PHONY : test clean

all: libmyQueue.so ./progetto

Queue.o: $(SRCDIR)/Queue.c
				$(CC) $(CFLAGS) $^ -c -fPIC -o $(SRCDIR)/$@

libmyQueue.so: $(SRCDIR)/Queue.o
							$(CC) -shared -o $(LIBDIR)/$@ $^

./progetto: ./supermarketProject.c
					$(CC) $(CFLAGS) $^ -o $@ $(LIBLINK)

test: 
			./progetto $(TESTDIR)/test.txt &
			sleep 25s 
			#cat progetto | xargs killall -1
			sleep 5s
			killall -1 progetto
			chmod +x ./analisi.sh 
			./analisi.sh logFile.log 

clean: 
			rm -f ./progetto ./logFile.log $(LIBDIR)/libmyQueue.so $(SRCDIR)/Queue.o
					