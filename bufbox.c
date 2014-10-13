/*
 * BUFBOX: caja de buffers
 * de tamaño max
 * Soporta solo un productor y un consumidor por caja
 */

#include "bufbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#define min(a,b) ((a) < (b) ? (a) : (b))

BUFBOX *create_bufbox(int max) {
    BUFBOX *box;
    int i;
    char name_vacios[20];
    char name_llenos[20];

    box = (BUFBOX *)malloc(sizeof(BUFBOX));
/*
    sem_init(&box->vacios, 0, max);
    sem_init(&box->llenos, 0, 0);
*/

    for(i=0; i<100; i++) {
	sprintf(name_vacios, "/sem_vacios%d", i);
	sprintf(name_llenos, "/sem_llenos%d", i);
        box->vacios = sem_open(name_vacios, O_RDWR|O_CREAT|O_EXCL, 0777, max);
        if(box->vacios == SEM_FAILED)
	    continue;
        box->llenos = sem_open(name_llenos, O_RDWR|O_CREAT|O_EXCL, 0777, 0);
        if(box->llenos == SEM_FAILED) {
	    sem_unlink(name_vacios);
	    continue;
        }
        sem_unlink(name_llenos); sem_unlink(name_vacios);
	break;
    }
    if(i==100) {
	fprintf(stderr, "no sem!\n");
	exit(1);
    }

    pthread_mutex_init(&box->mutex, NULL);
    box->state = BOX_OPEN;
    box->in = box->out = 0;
    box->cnt = 0;
    box->max = max;
    box->sizes = (int *)malloc(max*sizeof(int));
    box->bufs = (char **)malloc(max*sizeof(char *));
    if(box->sizes == NULL || box->bufs == NULL) {
	fprintf(stderr, "no mem!\n");
	exit(1);
    }
    return box;
}

void close_bufbox(BUFBOX *box) {
    pthread_mutex_lock(&box->mutex);
    box->cnt++;
    box->state = BOX_CLOSED;
    sem_post(box->llenos);
    pthread_mutex_unlock(&box->mutex);
}

void delete_bufbox(BUFBOX *box) {
    int i;

/*
    sem_destroy(&box->vacios);
    sem_destroy(&box->llenos);
*/
    sem_close(box->vacios);
    sem_close(box->llenos);
    free(box->bufs); free(box->sizes);
}

void putbox(BUFBOX *box, char *buf, int n) {
    if(box->state == BOX_CLOSED) return;
    sem_wait(box->vacios);
    if(box->state == BOX_CLOSED) {
	sem_post(box->llenos); return;
    }

    if(n > 0) { /* Ojo con el buffer de tamaño cero, debe funcionar */
        box->bufs[box->in] = malloc(n);
        memcpy(box->bufs[box->in], buf, n);
    } else box->bufs[box->in] = NULL;
    box->sizes[box->in] = n;
    box->in = (box->in+1)%box->max;
    pthread_mutex_lock(&box->mutex);
    box->cnt++;
    pthread_mutex_unlock(&box->mutex);
    sem_post(box->llenos);
}

int getbox(BUFBOX *box, char *buf, int n) {
    sem_wait(box->llenos);
    if(box->cnt == 1 && box->state == BOX_CLOSED) {
    	pthread_mutex_lock(&box->mutex);
    	box->cnt--;
    	pthread_mutex_unlock(&box->mutex);
    	sem_post(box->vacios);
	return -1;
    }
    n = min(box->sizes[box->out], n);
    if(n > 0) {
    	memcpy(buf, box->bufs[box->out], n);
    	free(box->bufs[box->out]);
    }
    box->out = (box->out+1)%box->max;
    pthread_mutex_lock(&box->mutex);
    box->cnt--;
    pthread_mutex_unlock(&box->mutex);
    sem_post(box->vacios);
    return n;
}

int boxsz(BUFBOX *box) {
    return box->cnt;
}
