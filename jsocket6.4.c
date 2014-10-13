#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <strings.h> 
#include <string.h>
#include <errno.h>

#include "jsocket6.4.h"

/* 
 * ToDo: Revisar portabilidad con endians de codigo JOLD_STYLE de IPV4_MAPPED
 */



/*
 * Version 2.0: con soporte transparente para IPv6
 *              API full compatible con los viejos jsockets
 *              Autores: Jo Piquer con ayuda de Sebastian Kreft, 2009
 * Version 2.1: Con mejor soporte para equipos no-IPv6
 * Version 3.0: Con soporte para gethostbyname2_r para ser reentrante
 * Version 4.0: Nueva API: j_sock_connect y j_sock_bind
 */

/* Si se prefiere evitar getaddrinfo (hay versiones con bugs en Linux) */
// #define JOLD_STYLE  /* beta testing now: No usar si no es en Linux! */

/* Si se quiere debugging al intentar direcciones IP (Trying....) */
#define JDEBUG

/* Si queremos IPv6 junto con IPv4 */
#define JIPv6

/*
 * Parte I: 
 * API Antigua: j_socket(), j_connect(), j_bind()
 * Se recomienda usar la nueva API, es más compatible con IPv6
 */

/*
 * Retorna un socket para conexion
 * En dual-stack, usamos un socket IPv6 y IPv4 MAPPED para soportar IPv4
 */


int j_socket()
{
    int sz = 1;
    int fd; 

#ifdef JIPv6
    	fd = socket(PF_INET6, SOCK_STREAM, 0);
#else
	fd = socket(PF_INET, SOCK_STREAM, 0);
#endif
    
    if(fd < 0) { fprintf(stderr, "no pude hacer un socket\n");
		  return -1;
	       }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &sz, 4);
    return(fd);
}


/*
 * Pone un "nombre" (port) a un socket
 * y lo prepara para recibir conexiones
 * retorna 0 si OK, -1 si no
 * En dual-stack hacemos bind en IPv6 y usamos IPv4 MAPPED para IPv4
 */

int j_bind(s, port)
int s;
int port;
{
#ifdef JIPv6
    struct sockaddr_in6 portname;
#else
    struct sockaddr_in portname;
#endif

    /* ponemos el nombre */
    bzero(&portname, sizeof portname); 
#ifdef JIPv6
        portname.sin6_port = htons(port);
        portname.sin6_family = AF_INET6;
        portname.sin6_addr = in6addr_any;
#else
        portname.sin_port = htons(port);
        portname.sin_family = AF_INET;
        portname.sin_addr.s_addr = INADDR_ANY;
#endif

    /* lo asociamos al socket */
    if( bind(s, (struct sockaddr *) &portname, sizeof portname) != 0)
            return(-1);

    listen(s, 5);
    return(0);
}

/*
 * Se conecta con un port conocido
 * retorna 0 si OK, -1 si no
 * Debiera usar getaddrinfo() y listo, pero no me funciona bien
 * en algunos linuxes
 * JOLD_STYLE implementa esto a mano y funciona bien en Linux/386
 */

#define MAX_NAME INET6_ADDRSTRLEN

int j_connect(s, host, port)
int s;
char *host;
int port;
{
        struct addrinfo hints;
	struct addrinfo *addresses, *hp;
#ifdef JOLD_STYLE
	struct hostent hostbuf, *hp2 = NULL;
	size_t hstbuflen;
	char *tmphstbuf;
#ifdef JIPv6
        struct sockaddr_in6 portname;
#else
        struct sockaddr_in portname;
#endif
	int i;
	int res, herr;
#endif /* JOLD_STYLE */
	char sport[20];
  	int ret;
 	char name[MAX_NAME];


#ifdef JOLD_STYLE
	hstbuflen = 1024;
	tmphstbuf = malloc(hstbuflen);
#ifdef JIPv6
	while((res=gethostbyname2_r(host, AF_INET6, &hostbuf, tmphstbuf,
 				    hstbuflen, &hp2, &herr)) == ERANGE) {
	    /* Need more buffer */
	    hstbuflen *= 2;
	    tmphstbuf = realloc(tmphstbuf, hstbuflen);
	}

	if(res == 0 && hp2 != NULL) {
	    bzero(&portname, sizeof portname);
	    portname.sin6_port = htons(port);
	    portname.sin6_family = AF_INET6;

            /* Trato de conectarme con todas las direcciones IP del servidor */
            for(i=0; hp2->h_addr_list[i] != NULL; i++) {
                bcopy(hp2->h_addr_list[i], &portname.sin6_addr.s6_addr, hp2->h_length);

#ifdef JDEBUG
	        inet_ntop(AF_INET6, portname.sin6_addr.s6_addr, name, sizeof name); 
	        fprintf(stderr, "Trying %s/%d...", name, port);
#endif
                if(connect(s, (struct sockaddr *)&portname, sizeof portname) == 0) {
		    fprintf(stderr, "done\n");
		    free(tmphstbuf);
                    return(0);
		}
		fprintf(stderr, "failed\n");
	    }
        }
#endif
        /* No logre' conectarme en IPv6, usamos IPv4-mapeado */

	while((res=gethostbyname2_r(host, AF_INET, &hostbuf, tmphstbuf,
 				    hstbuflen, &hp2, &herr)) == ERANGE) {
	    /* Need more buffer */
	    hstbuflen *= 2;
	    tmphstbuf = realloc(tmphstbuf, hstbuflen);
	}

	if(res == 0 && hp2 != NULL) {
	    bzero(&portname, sizeof portname);
#ifdef JIPv6
	    portname.sin6_port = htons(port);
	    portname.sin6_family = AF_INET6;
#else
	    portname.sin_port = htons(port);
	    portname.sin_family = AF_INET;
#endif

            /* Trato de conectarme con todas las direcciones IP del servidor */
            for(i=0; hp2->h_addr_list[i] != NULL; i++) {
#ifdef JIPv6
		uint32_t *p = portname.sin6_addr.s6_addr32;
/* mapear dirs IPv4 a IPv6!! */
                bcopy(hp2->h_addr_list[i], p+3, hp2->h_length);
		p[0] = 0; p[1] = 0; p[2] = htonl(0xffff);
#ifdef JDEBUG
	        inet_ntop(AF_INET6, portname.sin6_addr.s6_addr, name, sizeof name); 
	        fprintf(stderr, "Trying %s/%d...", name, port);
#endif
#else /* JIPv6 */
		bcopy( hp2->h_addr_list[i], &portname.sin_addr.s_addr, hp2->h_length);
#ifdef JDEBUG   
	        inet_ntop(AF_INET, &portname.sin_addr.s_addr, name, sizeof name); 
		fprintf(stderr, "Trying %s/%d...", name, port);
#endif
#endif /* JIPv6 */

                if(connect(s, (struct sockaddr *)&portname, sizeof portname) == 0) {
		    fprintf(stderr, "done\n");
		    free(tmphstbuf);
                    return(0);
		}
		else { perror("connect"); fprintf(stderr, "sock failed=%d\n", s);}
	    }
        } else fprintf(stderr, "name fail!: %s\n", host);

        /* No logre' conectarme de ninguna forma */
	free(tmphstbuf);
	return -1;
#else /* JOLD_STYLE */
        sprintf(sport, "%d", port); 
        /* Traducir nombre a direccion IP */
	memset(&hints, 0, sizeof(struct addrinfo));
#ifdef JIPv6
	hints.ai_family = AF_INET6;
#else
	hints.ai_family = AF_INET;
#endif
	hints.ai_socktype = SOCK_STREAM;
	// hints.ai_flags = AI_NUMERICSERV; /*  | AI_IDN; */
	hints.ai_flags = AI_V4MAPPED|AI_ALL;  /* Retornar todo IPv4 en IPv6 */
		    // | AI_ADDRCONFIG   /* retornar IPv6 solo si yo tengo: no funciona bien */
                   /*  | AI_IDN;   *//* Aceptar nombres IDN: no estandar aun */

	ret = getaddrinfo(host, sport, &hints, &addresses);

	if( ret != 0 ) /* Mmmh, puede ser que no soporte bien IPv6? */
	  {
	    fprintf(stderr, "Name/port unknown: %s/%s err: %s\n", host, sport, gai_strerror(ret));
	    return(-1);
	  }
	
        /* Trato de conectarme con todas las direcciones IP del servidor */ 
	for(hp=addresses; hp != NULL; hp = hp->ai_next) {
#ifdef JDEBUG
	    getnameinfo(hp->ai_addr, hp->ai_addrlen, name, MAX_NAME, NULL, 0, NI_NUMERICHOST);
	    fprintf(stderr, "Trying %s/%s...", name, sport);
#endif
	    if(connect(s, hp->ai_addr, hp->ai_addrlen) == 0) {
		fprintf(stderr, "done\n");
                break;
	    }
	    fprintf(stderr, "failed\n");
        }                                                 

	freeaddrinfo(addresses);

    	if(hp == NULL)
        /* No logre' conectarme */
            return -1;
	else
	    return 0;
#endif /* JOLD_STYLE */
}

/*
 * Acepta una conexion pendiente o se bloquea esperando una
 * retorna un fd si OK, -1 si no
 * sockaddr_storage soporta IPv4 e IPv6
 * Válida para la nueva API también
 */
int j_accept(s)
int s;
{
    struct sockaddr_storage from;

    unsigned int size = sizeof from;

    return( accept(s, (struct sockaddr *) &from, &size) );
}


/*
 * Parte II: 
 * API Nueva: j_socket_bind(), j_socket_connect()
 * Se recomienda usar la nueva API, es más compatible con IPv6
 */

int j_socket_udp_bind(char *port) {
	return j_socket_bind(SOCK_DGRAM, port);
}

int j_socket_tcp_bind(char *port) {
	return j_socket_bind(SOCK_STREAM, port);
}

int j_socket_bind(int type, char *port) {
    struct addrinfo hints, *res, *ressave;
    int n, sockfd;

    memset(&hints, 0, sizeof(struct addrinfo));

 /*
 *       AI_PASSIVE flag: the resulting address is used to bind
 *      to a socket for accepting incoming connections.
 *      So, when the hostname==NULL, getaddrinfo function will
 *      return one entry per allowed protocol family containing
 *      the unspecified address for that family.
 *   
 */

    hints.ai_flags    = AI_PASSIVE | AI_V4MAPPED | AI_ALL;
#ifdef JIPv6
    hints.ai_family   = AF_INET6;
#else
    hints.ai_family   = AF_INET;
#endif
    hints.ai_socktype = type;

    n = getaddrinfo(NULL, port, &hints, &res);

    if (n <0) {
        fprintf(stderr,
                "getaddrinfo error:: [%s]\n",
                gai_strerror(n));
        return -1;
    }

    ressave=res;

/*
 *        Try open socket with each address getaddrinfo returned,
 *               until getting a valid listening socket.
 */

    sockfd=-1;
    while (res) {
        sockfd = socket(res->ai_family,
                        res->ai_socktype,
                        res->ai_protocol);

        if (!(sockfd < 0)) {
            if (bind(sockfd, res->ai_addr, res->ai_addrlen) == 0)
                break;

            close(sockfd);
            sockfd=-1;
        }
        res = res->ai_next;
    }

    if (sockfd < 0) {
        freeaddrinfo(ressave);
        fprintf(stderr,
                "socket error:: could not open socket\n");
        return -1;
    }

    if(type == SOCK_STREAM) listen(sockfd, 5);

    freeaddrinfo(ressave);

    return sockfd;
}

int j_socket_udp_connect(char *server, char *port) {
    return j_socket_connect(SOCK_DGRAM, server, port);
}

int j_socket_tcp_connect(char *server, char *port) {
    return j_socket_connect(SOCK_STREAM, server, port);
}

int j_socket_connect(int type, char *server, char *port) {
    struct addrinfo hints, *res, *ressave;
    int n, sockfd;

    memset(&hints, 0, sizeof(struct addrinfo));

    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = type;
    hints.ai_flags    = AI_V4MAPPED | AI_ALL;

    n = getaddrinfo(server, port, &hints, &res);

    if (n <0) {
        fprintf(stderr,
                "getaddrinfo error:: [%s]\n",
                gai_strerror(n));
        return -1;
    }

    ressave=res;

/*
 *        Try open socket with each address getaddrinfo returned,
 *               until getting a valid listening socket.
 */
    sockfd=-1;
    while (res != NULL) {
        sockfd = socket(res->ai_family,
                        res->ai_socktype,
                        res->ai_protocol);

        if (!(sockfd < 0)) {
            if (connect(sockfd, res->ai_addr, res->ai_addrlen) == 0)
                break;

            close(sockfd);
            sockfd=-1;
        }
        res = res->ai_next;
    }

    freeaddrinfo(ressave);

    return sockfd;
}
