#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <fcntl.h>
#include "jsocket6.4.h"
#include "Data.h"

#define BUFFER_LENGTH 1400
#define PORT "2000" 

char buffer[BUFFER_LENGTH];

int main(int argc, char **argv) {
    int s;
    int n;
    int i, cnt;
    double t;
    struct timeval t0, t1, t2;
    char *server, *file, *file2;
    int fd, fd2;
   
    if(argc == 1) {
	fprintf(stderr, "Use: bwc filename1 filename2 [servername]\n");
	return 1;
    }

    if(argv[1][0] == '-') {
	Data_debug = 1;
	argv++; argc--;
    }

    if(argc == 3) {
	file = argv[1];
	file2 = argv[2];
	server = "localhost";
    }
    else if(argc == 4) {
	file = argv[1];
	file2 = argv[2];
	server = argv[3];
    }
    else {
	fprintf(stderr, "Use: bwc filename1 filename2 [servername]\n");
	return 1;
    }

    fd = open(file, O_RDONLY);
    if(fd < 0) {
	fprintf(stderr, "Cannot open: %s\n", file);
	perror("open");
	exit(1);
    }

    fd2 = open(file2, O_WRONLY|O_CREAT|O_TRUNC,0664);
    if(fd2 < 0) {
	fprintf(stderr, "Cannot open for writing: %s\n", file2);
	perror("open");
	exit(1);
    }

    s = Dconnect(server, PORT);
    if(s < 0) {
	printf("connect failed\n");
       	exit(1);
    }

    printf("conectado\n");

    gettimeofday(&t0, NULL);
    for(i=0;;i+=cnt) {
	if((cnt=read(fd, buffer, BUFFER_LENGTH)) <= 0)
	    break;
        Dwrite(s, buffer, cnt);
    }
    Dwrite(s, buffer, 0);

    gettimeofday(&t1, NULL);
    t = (t1.tv_sec*1.0+t1.tv_usec*1e-6) - (t0.tv_sec*1.0+t0.tv_usec*1e-6); 
    printf("write at %g Mbps\n", i*8/1024/1024/t);

    for(i=0;; i+=cnt) {
        cnt = Dread(s, buffer, BUFFER_LENGTH);
	if(i == 0) gettimeofday(&t1, NULL);
	if(cnt <= 0)
	    break;
	write(fd2, buffer, cnt);
    }
    gettimeofday(&t2, NULL);
    Dclose(s);
    close(fd);
    close(fd2);

    t = (t2.tv_sec*1.0+t2.tv_usec*1e-6) - (t1.tv_sec*1.0+t1.tv_usec*1e-6); 
    if(i == 0) fprintf(stderr, "read 0 bytes!!\n");
    else
    printf("read %d bytes in %g seconds at %g Mbps\n\n", i, t, i*8/1024/1024/t);

    t = (t2.tv_sec*1.0+t2.tv_usec*1e-6) - (t0.tv_sec*1.0+t0.tv_usec*1e-6); 
    printf("throughput total: %d bytes in %g seconds at %g Mbps\n", i*2, t, i*2*8/1024/1024/t);

    sleep(1);
    exit(0);
}
