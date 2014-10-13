#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#ifdef __MACH__
#include <mach/clock.h>
#include <mach/mach.h>
#endif
#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>
#include <string.h>
#include "jsocket6.4.h"
#include "bufbox.h"
#include "Data-seqn.h"

#define MAX_QUEUE 100 /* buffers en boxes */

/* Version con threads rdr y sender
 * Implementa Stop and Wait sin seqn, falla frente a algunos errores
 * Modificado para incluir un número de secuencia
 */


int Data_debug = 0; /* para debugging */

/* Variables globales del cliente */
static int Dsock = -1;
static pthread_mutex_t Dlock;
static pthread_cond_t  Dcond;
static pthread_t Drcvr_pid, Dsender_pid;
static unsigned char ack[DHDR] = {0, ACK, 0};

static void *Dsender(void *ppp);
static void *Drcvr(void *ppp);


#define max(a, b) (((a) > (b))?(a):(b))

struct {
    BUFBOX *rbox, *wbox; /* Cajas de comunicación con el thread "usuario" */
    unsigned char pending_buf[BUF_SIZE]; /* buffer esperando ack */
    int pending_sz;  			 /* tamaño buffer esperando */
    int expecting_ack;			 /* 1: esperando ack */
    char expected_seq, expected_ack;    /* 0 o 1 */
    int retries;                         /* cuantas veces he retransmitido */
    double timeout;                      /* tiempo restante antes de retransmision */
    int state;                           /* FREE, CONNECTED, CLOSED */
    int id;                              /* id conexión, la asigna el servidor */
} connection;

/* Funciones utilitarias */

/* retorna hora actual */
double Now() {
    struct timespec tt;
#ifdef __MACH__ // OS X does not have clock_gettime, use clock_get_time
    clock_serv_t cclock;
    mach_timespec_t mts;
    host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
    clock_get_time(cclock, &mts);
    mach_port_deallocate(mach_task_self(), cclock);
    tt.tv_sec = mts.tv_sec;
    tt.tv_nsec = mts.tv_nsec;
#else
    clock_gettime(CLOCK_REALTIME, &tt);
#endif

    return(tt.tv_sec+1e-9*tt.tv_nsec);
}

/* Inicializa estructura conexión */
int init_connection(int id) {
    int cl;
    pthread_t pid;
    int *p;

    connection.state = CONNECTED;
    connection.wbox = create_bufbox(MAX_QUEUE); 
    connection.rbox = create_bufbox(MAX_QUEUE);
    connection.pending_sz = -1;
    connection.expecting_ack = 0;
    connection.expected_seq = 1;
    connection.expected_ack = 0;
    connection.id = id;
    return id;
}

/* borra estructura conexión */
void del_connection() {
    delete_bufbox(connection.wbox);
    delete_bufbox(connection.rbox);
    connection.state = FREE;
}

/* Función que inicializa los threads necesarios: sender y rcvr */
static void Init_Dlayer(int s) {

    Dsock = s;
    if(pthread_mutex_init(&Dlock, NULL) != 0) fprintf(stderr, "mutex NO\n");
    if(pthread_cond_init(&Dcond, NULL) != 0)  fprintf(stderr, "cond NO\n");
    pthread_create(&Dsender_pid, NULL, Dsender, NULL);
    pthread_create(&Drcvr_pid, NULL, Drcvr, NULL);
}

/* timer para el timeout */
void tick() {
    return;
}

/* Función que me conecta al servidor e inicializa el mundo */
int Dconnect(char *server, char *port) {
    int s, cl, i;
    struct sigaction new, old;
    unsigned char inbuf[DHDR], outbuf[DHDR];

    if(Dsock != -1) return -1;

    s = j_socket_udp_connect(server, port); /* deja "conectado" el socket UDP, puedo usar recv y send */
    if(s < 0) return s;

/* inicializar conexion */
    bzero(&new, sizeof new);
    new.sa_flags = 0;
    new.sa_handler = tick;
    sigaction(SIGALRM, &new, &old);

    outbuf[DTYPE] = CONNECT;
    outbuf[DID] = 0;
    outbuf[DSEQ] = 0;
    for(i=0; i < RETRIES; i++) {
        send(s, outbuf, DHDR, 0);
	alarm(INTTIMEOUT);
	if(recv(s, inbuf, DHDR, 0) != DHDR) continue;
	if(Data_debug) fprintf(stderr, "recibo: %c, %d\n", inbuf[DTYPE], inbuf[DID]);
	alarm(0);
	if(inbuf[DTYPE] != ACK || inbuf[DSEQ] != 0) continue;
	cl = inbuf[DID];
	break;
    }
    sigaction(SIGALRM, &old, NULL);
    if(i == RETRIES) {
	fprintf(stderr, "no pude conectarme\n");
	return -1;
    }
fprintf(stderr, "conectado con id=%d\n", cl);
    init_connection(cl);
    Init_Dlayer(s); /* Inicializa y crea threads */
    return cl;
}

/* Lectura */
int Dread(int cl, char *buf, int l) {
int cnt;

    if(connection.id != cl) return -1;

    cnt = getbox(connection.rbox, buf, l);
    return cnt;
}

/* escritura */
void Dwrite(int cl, char *buf, int l) {
    if(connection.id != cl || connection.state != CONNECTED) return;

    putbox(connection.wbox, buf, l);
/* el lock parece innecesario, pero se necesita:
 * nos asegura que Dsender está esperando el lock o el wait
 * y no va a perder el signal! 
 */
    pthread_mutex_lock(&Dlock);
    pthread_cond_signal(&Dcond); 	/* Le aviso a sender que puse datos para él en wbox */
    pthread_mutex_unlock(&Dlock);
}

/* cierra conexión */
void Dclose(int cl) {
    if(connection.id != cl) return;

    close_bufbox(connection.wbox);
    close_bufbox(connection.rbox);
}

/*
 * Aquí está toda la inteligencia del sistema: 
 * 2 threads: receptor desde el socket y enviador al socket 
 */

/* lector del socket: todos los paquetes entrantes */
static void *Drcvr(void *ppp) { 
    int cnt;
    int cl, p;
    unsigned char inbuf[BUF_SIZE];
    int found;

/* Recibo paquete desde el socket */
    while((cnt=recv(Dsock, inbuf, BUF_SIZE, 0)) > 0) {
   	if(Data_debug)
	    fprintf(stderr, "recv: id=%d, type=%c, seq=%d\n", inbuf[DID], inbuf[DTYPE], inbuf[DSEQ]);
	if(cnt < DHDR) continue;

	cl = inbuf[DID];
   	if(cl != connection.id) continue;

	pthread_mutex_lock(&Dlock);
	if(inbuf[DTYPE] == CLOSE) {
	if(Data_debug) fprintf(stderr, "recibo cierre conexión %d, envío ACK\n", cl);
	    ack[DID] = cl;
	    ack[DTYPE] = ACK;
	    ack[DSEQ] = inbuf[DSEQ];
	    if(send(Dsock, ack, DHDR, 0) < 0) {
		perror("send"); exit(1);
	    }
	    if(inbuf[DSEQ] != connection.expected_seq) {
		pthread_mutex_unlock(&Dlock);
		continue;
	    }
	    connection.expected_seq = (connection.expected_seq+1)%2;
	    connection.state = CLOSED;
	    Dclose(cl);
	}
	else if(inbuf[DTYPE] == ACK && connection.state != FREE
		&& connection.expecting_ack && connection.expected_ack == inbuf[DSEQ]) {
	    if(Data_debug)
		fprintf(stderr, "recv ACK id=%d, seq=%d\n", cl, inbuf[DSEQ]);

	    connection.expecting_ack = 0;
	    if(connection.state == CLOSED) {
		/* conexion cerrada y sin buffers pendientes */
		del_connection();
	    }
	    pthread_cond_signal(&Dcond);
	}
	else if(inbuf[DTYPE] == DATA && connection.state == CONNECTED) {
	    if(Data_debug) fprintf(stderr, "rcv: DATA: %d, seq=%d, expected=%d\n", inbuf[DID], inbuf[DSEQ], connection.expected_seq);
	    if(boxsz(connection.rbox) >= MAX_QUEUE) { /* No tengo espacio */
		pthread_mutex_unlock(&Dlock);
		continue;
	    }
	/* envio ack en todos los otros casos */
	    ack[DID] = cl;
	    ack[DTYPE] = ACK;
	    ack[DSEQ] = inbuf[DSEQ];

	    if(Data_debug) fprintf(stderr, "Enviando ACK %d, seq=%d\n", ack[DID], ack[DSEQ]);

	    if(send(Dsock, ack, DHDR, 0) <0)
		perror("sendack");

	    if(inbuf[DSEQ] != connection.expected_seq) {
		pthread_mutex_unlock(&Dlock);
		continue;
	    }
            connection.expected_seq = (connection.expected_seq+1)%2;
	/* enviar a la cola */
	    putbox(connection.rbox, (char *)inbuf+DHDR, cnt-DHDR);
        }
	else if(Data_debug) {
	    fprintf(stderr, "descarto paquete entrante: t=%c, id=%d\n", inbuf[DTYPE], inbuf[DID]);
	}
	
	pthread_mutex_unlock(&Dlock);
    }
    fprintf(stderr, "fallo read en Drcvr()\n");
    return NULL;
}

double Dclient_timeout_or_pending_data() {
    int cl, p;
    double timeout;
/* Suponemos lock ya tomado! */

    timeout = Now()+20.0;
	if(connection.state == FREE) return timeout;

	if(boxsz(connection.wbox) != 0 && !connection.expecting_ack)
	/* data from client */
	    return Now();

        if(!connection.expecting_ack)
	    return timeout;

	if(connection.timeout <= Now()) return Now();
	if(connection.timeout < timeout) timeout = connection.timeout;
    return timeout;
}

/* Thread enviador y retransmisor */
static void *Dsender(void *ppp) { 
    double timeout;
    struct timespec tt;
    int p;
    int ret;

  
    for(;;) {
	pthread_mutex_lock(&Dlock);
        /* Esperar que pase algo */
	while((timeout=Dclient_timeout_or_pending_data()) > Now()) {
// fprintf(stderr, "timeout=%f, now=%f\n", timeout, Now());
// fprintf(stderr, "Al tuto %f segundos\n", timeout-Now());
	    tt.tv_sec = timeout;
// fprintf(stderr, "Al tuto %f nanos\n", (timeout-tt.tv_sec*1.0));
	    tt.tv_nsec = (timeout-tt.tv_sec*1.0)*1000000000;
// fprintf(stderr, "Al tuto %f segundos, %d secs, %d nanos\n", timeout-Now(), tt.tv_sec, tt.tv_nsec);
	    ret=pthread_cond_timedwait(&Dcond, &Dlock, &tt);
// fprintf(stderr, "volvi del tuto con %d\n", ret);
	}

	/* Revisar clientes: timeouts y datos entrantes */

	    if(connection.state == FREE) continue;
	    if(connection.expecting_ack) { /* retransmitir */

	    	if(Data_debug) fprintf(stderr, "TIMEOUT\n");

		if(++connection.retries > RETRIES) {
		    fprintf(stderr, "too many retries: %d\n", connection.retries);
	    	    del_connection();
		    exit(1);
		}
 	  	if(Data_debug) fprintf(stderr, "Re-send DATA %d, seq=%d\n", connection.pending_buf[DID], connection.pending_buf[DSEQ]);
	 	if(send(Dsock, connection.pending_buf, DHDR+connection.pending_sz, 0) < 0) {
		    perror("send2"); exit(1);
		}
		connection.timeout = Now() + TIMEOUT;

	    }
	    else if(boxsz(connection.wbox) != 0) { /* && !connection.expecting_ack) */
/*
		Hay un buffer para mi para enviar
		leerlo, enviarlo, marcar esperando ACK
*/
		connection.pending_sz = getbox(connection.wbox, (char *)connection.pending_buf+DHDR, BUF_SIZE); 
		connection.pending_buf[DID]=connection.id;
		connection.expected_ack = (connection.expected_ack+1)%2;
		connection.pending_buf[DSEQ]=connection.expected_ack;

		if(connection.pending_sz == -1) { /* EOF */ 
		if(Data_debug) fprintf(stderr, "sending EOF\n");
		   connection.state = CLOSED;
		   connection.pending_buf[DTYPE]=CLOSE;
		   connection.pending_sz = 0;
		}
		else {
		   if(Data_debug) 
			fprintf(stderr, "sending DATA id=%d, seq=%d\n", connection.id, connection.pending_buf[DSEQ]);
		   connection.pending_buf[DTYPE]=DATA;
		}
		
		send(Dsock, connection.pending_buf, DHDR+connection.pending_sz, 0);
		connection.expecting_ack = 1;
		connection.timeout = Now() + TIMEOUT;
		connection.retries = 0;
	    }
	pthread_mutex_unlock(&Dlock);
    }
    return NULL;
}
