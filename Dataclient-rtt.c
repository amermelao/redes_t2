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
#define SIZE_RTT 10 /* David: cantidad de RTTs a promediar */
#define MIN_TIMEOUT 0.005 /* Roberto: cota inferior timeout */
#define MAX_TIMEOUT 3.0 /* Roberto: cota superior timeout */
#define SWS 50 /* David: send window size */

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
static unsigned char ack[DHDR] = {0, ACK, 0, 0}; /* David: agregamos un byte para indicar retransmisiones */
double T1, T2; /* David: variables globales para calcular RTTs */
char LAR, LFS; /* David: LAR y LFS de Go-back-N */

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
    double rtt[SIZE_RTT];		 /* David: arreglo de RTTs históricos */
    double rtt_time[SIZE_RTT];           /* Roberto: se agrega un arregle que almacena el tiempo en que se agrega un nuevo elemento al arreglo*/
    double timeRef;                      /* Roberto: para tener una referencia del tiempo*/
    int rtt_idx;			 /* David: indicador de rtt a actualizar */
} connection;

/* David: estructura que almacena datos en caso de tener que retransmitir */
struct {
    unsigned char pending_buf[SWS][BUF_SIZE];
    int pending_sz[SWS];
} BackUp;

/* Funciones utilitarias */

/* David: almacenar paquetes para posible retransmisión en Go-back-N */
void wBackUp(unsigned char *pending_buf, int pending_sz, int index) {
	strcpy(BackUp.pending_buf[index],pending_buf);
	BackUp.pending_sz[index] = pending_sz;
}

/* David: RTT promedio de la ventana */
double getRTT() {
    int i;
    double peso;
    double sum = 0.0;
    double sumWeigth = 0;
    for(i=0; i<SIZE_RTT; i++) {
        peso = connection.rtt_time[i] - connection.timeRef;
	sum+=(connection.rtt[i]*peso);
        sumWeigth+=peso;
    }
    sum/=sumWeigth; /* Roberto: condiciones minimas y maximas de time out*/
    sum = sum*1.1<MIN_TIMEOUT ? MIN_TIMEOUT/1.1 : (sum*1.1>MAX_TIMEOUT ? MAX_TIMEOUT/1.1 : sum);
    return sum;
}

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
int init_connection(int id, double rtt,double timeRef, double timeNow) { /* David: se agrega 'rtt', que es el primer RTT calculado */
    int cl, i; /* David: variable 'i' agregada */
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
    connection.timeRef = timeRef;/*Roberto: se setea la primera referencia*/
    for(i=0; i<SIZE_RTT; i++) { /* David: se inicializa arreglo de RTTs */
	connection.rtt[i] = rtt;
        connection.rtt_time[i] = timeNow;/*Se agregan los primeros tiempos*/
    }
    connection.rtt_idx = 0; /* David: se inicializa indicador de rtt a actualizar en 0 */

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
    outbuf[DRET] = 0; /* David: agregamos retransmisiones */
    for(i=0; i < RETRIES; i++) {
	T1 = Now(); /* David: se inicia conteo */
        send(s, outbuf, DHDR, 0);
	alarm(INTTIMEOUT);
	if(recv(s, inbuf, DHDR, 0) != DHDR) continue;
	T2 = Now(); /* David: se finaliza conteo */
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
    printf("RTT calculado en inicio de conexión: %f\n",T2-T1);
    init_connection(cl,T2-T1,T1,T2); /* David: se pasa rtt=T2-T1 como parámetro, Robeto: y T1,T2, para el calculo de los pesos */
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
	T2 = Now(); /* David: tiempo de recepción */
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
	    ack[DRET] = inbuf[DRET]; /* David: se agregan retransmisiones al final de header */
	    if(send(Dsock, ack, DHDR, 0) < 0) {
		perror("send"); exit(1);
	    }
	    if(inbuf[DSEQ] != connection.expected_seq) {
		pthread_mutex_unlock(&Dlock);
		continue;
	    }
	    connection.expected_seq = (connection.expected_seq+1)%256; /* David: secuencias entre 0 y 255 (antes había un 2) */
	    connection.state = CLOSED;
	    Dclose(cl);
	}
	else if(inbuf[DTYPE] == ACK && connection.state != FREE
		&& connection.expecting_ack && ( (connection.expected_ack <= inbuf[DSEQ] && inbuf[DSEQ]-connection.expected_seq<50) || (connection.expected_seq >= inbuf[DSEQ] && connection.expected_seq-inbuf[DSEQ]>205) ) ) { /* David: se cambia "connection.expected_ack == inbuf[DSEQ]" al operador "<=" */
    	    /* David: medimos RTT cuando recibimos ACK */
	    if(~inbuf[DRET]) { /* Si no hay retransmisión */
                connection.timeRef = connection.rtt_time[connection.rtt_idx];/* Roberto: se cambia la referencia al valor que se esta lledo*/
                connection.rtt_time[connection.rtt_idx] = T2; /* Roberto: se pone el tiempo en que fue recibida la respuesta*/
	        connection.rtt[connection.rtt_idx] = T2-T1; /* David: guardamos nuevo RTT */
                connection.rtt_idx = (connection.rtt_idx+1)%SIZE_RTT; /* David: modificamos a posición de siguiente RTT a actualizar */
                
	    }

	    if(Data_debug)
		fprintf(stderr, "recv ACK id=%d, seq=%d\n", cl, inbuf[DSEQ]);

	    connection.expecting_ack = 0; /* ??? */
	    if(connection.state == CLOSED) {
		/* conexion cerrada y sin buffers pendientes */
		del_connection();
	    }

	    LAR = inbuf[DSEQ]; /* David: agregamos actualización del LAR */

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
	    ack[DRET] = inbuf[DRET]; /* David: se agregan retransmisiones al final de header */

	    if(Data_debug) fprintf(stderr, "Enviando ACK %d, seq=%d\n", ack[DID], ack[DSEQ]);

	    /* David: si exp_seq==seq, se manda ack_seq, sino ack_seq-1 */
	    if(inbuf[DSEQ] != connection.expected_seq) {
		ack[DSEQ] = (connection.expected_seq - 1)%256;
		if(send(Dsock, ack, DHDR, 0) <0)
                	perror("sendack");
		pthread_mutex_unlock(&Dlock);
		continue;
	    }
	    else {
		if(send(Dsock, ack, DHDR, 0) <0)
                	perror("sendack");
	    }

            connection.expected_seq = (connection.expected_seq+1)%256; /* David: número de secuencia entre 0 y 255 */
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

    timeout = Now() + getRTT()*1.1;/* David: antes era Now()+20.0 */
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
    int i;
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

		/* David: retransmisión de ventana */
		for(i=0; i<SWS; i++) {
 	  		if(Data_debug) fprintf(stderr, "Re-send DATA %d, seq=%d\n", BackUp.pending_buf[i][DID], BackUp.pending_buf[i][DSEQ]);
			BackUp.pending_buf[i][DRET]++; /* David: sumamos una retransmisión del paquete */
	 		if(send(Dsock, BackUp.pending_buf[i], DHDR+BackUp.pending_sz[i], 0) < 0) {
		    		perror("send2"); exit(1);
			}
			connection.timeout = Now() + getRTT()*1.1;
		}

	    }
	    else if(boxsz(connection.wbox) != 0) { /* && !connection.expecting_ack) */
/*
		Hay un buffer para mi para enviar
		leerlo, enviarlo, marcar esperando ACK
*/

		connection.expected_ack = (LAR + 1)%256; /* David: (connection.expected_ack+1)%2; */

		for(i=0; i<SWS; i++) {

			connection.pending_sz = getbox(connection.wbox, (char *)connection.pending_buf+DHDR, BUF_SIZE); 
			connection.pending_buf[DID]=connection.id;
			connection.pending_buf[DSEQ]=(connection.expected_ack+1)%256; /* David: número de secuencia entre 0 y 255 */
			connection.pending_buf[DRET]=0; /* David: inicializamos retransmisiones en 0 */

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
		
			if(i == 0) /* David: si es el primer envío de la ventana */
				T1 = Now(); /* David: tiempo inicial primer envío */
			send(Dsock, connection.pending_buf, DHDR+connection.pending_sz, 0);
			connection.timeout = Now() + getRTT()*1.1;

			/* David: se almacena paquete por si hay que retransmitir */
			wBackUp(connection.pending_buf, connection.pending_sz, i);

		}
		connection.expecting_ack = 1;
		connection.retries = 0;
		LFS = (LAR + SWS - 1)%SWS; /* David: actualizamos LFS */
	    }
	pthread_mutex_unlock(&Dlock);
    }
    return NULL;
}
