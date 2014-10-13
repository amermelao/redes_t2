# Para Linux agregar -lrt
CC=gcc
CFLAGS=-g # -m32

BIN=bwc-orig bwc 

all: $(BIN)

# Uds deben escribir y proveer Dataclient-rtt.c a partir de Dataclient-seqn.c
bwc-orig: bwc.o jsocket6.4.o Dataclient-seqn.o jsocket6.4.h bufbox.o 
	$(CC) $(CFLAGS) bwc.o jsocket6.4.o Dataclient-seqn.o bufbox.o -o $@ -lpthread -lrt

bwc: bwc.o jsocket6.4.o Dataclient-rtt.o jsocket6.4.h bufbox.o 
	$(CC) $(CFLAGS) bwc.o jsocket6.4.o Dataclient-rtt.o bufbox.o -o $@ -lpthread -lrt

cleanall: 
	rm -f $(BIN) *.o
