/* Interfaz antigua */
int j_socket();
int j_bind(int, int);
int j_connect(int, char *, int);
/* compatible con ambas */
int j_accept(int);

/* Nueva Interfaz */
/* soporta udp y tcp
 * junta j_socket con bind y con connect para compatibilidad con
 * llamadas a getaddrinfo()
 */

/* OJO: port es string ahora */

int j_socket_udp_bind(char *port);
int j_socket_tcp_bind(char *port);
int j_socket_bind(int type, char *port);

int j_socket_udp_connect(char *server, char *port);
int j_socket_tcp_connect(char *server, char *port);
int j_socket_connect(int type, char *server, char *port);
