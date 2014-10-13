#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include "Data.h"

#define BUFFER_LENGTH 1400
#define PORT "2000"

char buffer[BUFFER_LENGTH];

void mclock() {
}

void* server(void *pcl);

int main(int argc, char **argv) {
    if(argc == 2) Data_debug = 1;

    Dbind(server, PORT); /* no retorna */
}


void* server(void *pcl) {
    int n, nl;
    int i, cnt;
    struct timeval t0, t1, t2;
    double t;
    char tmpfilename[15];
    int fd;
    struct sigaction new;
    int cl;

    cl = *((int *)pcl);
    free(pcl);

    bzero(&new, sizeof new);
    new.sa_flags = 0;
    new.sa_handler = mclock;
    sigaction(SIGALRM, &new, NULL);

    strcpy(tmpfilename, "tmpbwXXXXXX");

    fd = mkstemp(tmpfilename);
    if(fd < 0) {
	fprintf(stderr, "Can't create temp %s\n", tmpfilename);
	exit(1);
    }
    if(!Data_debug) unlink(tmpfilename);
    else fprintf(stderr, "debug: dejo archivo temporal %s\n", tmpfilename);

    fprintf(stderr, "cliente conectado\n");

    gettimeofday(&t0, NULL);
    for(i=0;; i+=cnt) {
	cnt = Dread(cl, buffer, BUFFER_LENGTH);
	if(cnt <= 0) break;
	write(fd, buffer, cnt);
    }

    gettimeofday(&t1, NULL);
    t = (t1.tv_sec+t1.tv_usec*1e-6)-(t0.tv_sec+t0.tv_usec*1e-6); 
    printf("read at %g Mbps\n", i*8/1024/1024/t);

    n = lseek(fd, 0, SEEK_CUR);
    lseek(fd, 0, SEEK_SET);

    for(i=0; i <= n-BUFFER_LENGTH; i+=BUFFER_LENGTH) {
	if(read(fd, buffer, BUFFER_LENGTH) != BUFFER_LENGTH) {
	    fprintf(stderr, "premature EOF!!!\n");
	    exit(1);
	}
        Dwrite(cl, buffer, BUFFER_LENGTH);
    }

    if(n-i > 0) {
	if(read(fd, buffer, n-i) != n-i) {
	    fprintf(stderr, "premature EOF!!!\n");
	    exit(1);
	}
        Dwrite(cl, buffer, n-i);
    }

    Dwrite(cl, buffer, 0);

    gettimeofday(&t2, NULL);
    t = (t2.tv_sec+t2.tv_usec*1e-6)-(t1.tv_sec+t1.tv_usec*1e-6); 
    printf("write at %g Mbps\n", n*8/1024/1024/t);
    close(fd);
    Dclose(cl);
    return NULL;
}
