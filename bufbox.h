/*
 * BUFBOX: caja de buffers
 * de tamaño max
 * Soporta solo un productor y un consumidor por caja
 */

#include <pthread.h>
#include <semaphore.h>

/* para state */
#define BOX_OPEN 1
#define BOX_CLOSED 2

typedef struct {
    sem_t *vacios, *llenos; 
    pthread_mutex_t mutex;
    char **bufs;
    int  *sizes;
    int in, out;
    int cnt;
    int max;
    int state;
} BUFBOX;

BUFBOX *create_bufbox(int max);
void delete_bufbox(BUFBOX *box);
void putbox(BUFBOX *box, char *buf, int n);
int getbox(BUFBOX *box, char *buf, int n);
void close_bufbox(BUFBOX *box);
int boxsz(BUFBOX *box);
