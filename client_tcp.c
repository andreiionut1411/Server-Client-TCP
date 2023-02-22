#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#define PAYLOAD_LEN 1501 // Avem loc si pentur null terminator
#define MSG_LEN 1562 // 2 pentru len, 2 pentru type, 4 pentru ip, 2 pentru port
                     // 50 pentru topic, 1 pentru data_type,
                     // maxim 1500 pentru payload si 1 pentru \0.

#define INT 0
#define SHORT_REAL 1
#define FLOAT 2
#define STRING 3
#define NAME_TYPE 0
#define EXIT_TYPE 1
#define SUBSCRIBE_TYPE 2
#define UNSUBSCRIBE_TYPE 3
#define TOPIC_TYPE 4
#define SUB_SUCCESS 2 // Acest tip de mesaj va fi dat de server daca s-a facut subscribe.
                      // Daca nu s-a reusit, atunci server-ul nu transmite nimic.
#define UNSUB_SUCCESS 3
#define SF_TRUE 1
#define SF_FALSE 0

int chestie = 0;

struct message{
    uint16_t len;
    uint16_t type; // E uint16 pentru a evita padding-ul automat facut de C.
    uint32_t ip; // Va fi nevoie pentru a transmite IP-ul clientului UDP.
    uint16_t port; 
    char topic[50];
    char data_type;
    char payload[PAYLOAD_LEN];
};

char buffer[MSG_LEN];

int interpret_int (char *payload){
    char sign = payload[0];
    int number = ntohl (*(int *)(payload + 1));

    if (sign == 1) 
        number = -number;
    
    return number;
}

float interpret_short_real (char *payload){
    uint16_t number = ntohs (* (uint16_t *)payload);
    float res = ((float) number) / 100;

    return res;
}

// Vom folosi %g la afisare pentru a elimina orice trailing zero, dar pentru
// a face afisarea corect, trebuie sa ii dam ca parametru cu ce precizie sa
// faca afisarea. Precizia va fi numarul de cifre al numarului.
float interpret_float (char *payload, int* precision){
    char sign = payload[0];
    uint32_t number = ntohl (*(uint32_t *) (payload + 1));
    char power = payload[5];
    float res = number;

    // Precizia ne va indica cu cate zecimale dupa virgula sa scriem numarul.
    *precision = 0;
    while (power > 0){
        res /= 10;
        (*precision)++;
        power--;
    }

    if (sign == 1){
        res = -res;
    }

    return res;
}

// Trimite ce este in buffer la un socket. Daca este un mesaj mai important,
// de exemplu mesjul de inchidere a clientului TCP, atunci se incearca de mai
// multe ori trimiterea daca primele incercari esueaza.
void send_buffer(int sockfd, int len, int tries){
    int sent = 0;

    while (len > 0){
        // Incercam sa trimitem pana se trimite tot mesajul
        int ret = send (sockfd, buffer + sent, len, 0);
        if (ret < 0){
            fprintf (stderr, "A aparut o eroare la transmisie.\n");
            tries--;
        }

        if (tries < 0)
            return;

        sent += ret;
        len -= ret;
    }
}

// Dupa ce s-a facut conexiunea, clientul TCP isi va trimite ID-ul, si astfel
// se va incheia etapa de initializare a legaturii si poate incepe schimbul
// de mesaje intre client si server.
void send_name (int sockfd, char *id){
    struct message hello_msg;
    uint16_t len;
    int ret, sent = 0;

    // In payload vom avea id-ul.
    hello_msg.len = htons (3 * sizeof (uint16_t) + 51 * sizeof (char) +
                           sizeof (uint32_t) + strlen (id));
    len = ntohs (hello_msg.len);
    hello_msg.type = NAME_TYPE;
    hello_msg.ip = 0;
    hello_msg.port = 0;
    hello_msg.data_type = 0;
    for (int i = 0; i < 50; i++){
        hello_msg.topic[i] = 0;
    }

    strcpy (hello_msg.payload, id);
    memcpy (buffer, (char *)&hello_msg, len);
    send_buffer (sockfd, len, 0);
}

void print_message (struct message* msg){
    struct in_addr in;
    in.s_addr = msg->ip;
    char data_type = msg->data_type;
    msg->data_type = '\0';

    if (data_type == INT){
        fprintf (stdout, "%s:%d - %s - INT - %d\n", inet_ntoa (in),
                 ntohs (msg->port), msg->topic, interpret_int (msg->payload));
    }
    else if (data_type == SHORT_REAL){
        fprintf (stdout, "%s:%d - %s - SHORT_REAL - %.2f\n", inet_ntoa (in),
                 ntohs (msg->port), msg->topic,
                 interpret_short_real (msg->payload));
        // Am folosit %g pentru a scapa de trailing zeros.
    }
    else if (data_type == FLOAT){
        int precision = 0;
        float number = interpret_float (msg->payload, &precision);
        fprintf (stdout, "%s:%d - %s - FLOAT - %.*lf\n", inet_ntoa (in),
                 ntohs (msg->port), msg->topic, precision, number);
    }
    else if (data_type == STRING){
        fprintf (stdout, "%s:%d - %s - STRING - %s\n", inet_ntoa (in),
                 ntohs (msg->port), msg->topic, msg->payload);
    }
}

int print_type_of_message (struct message* mesaj, int sockfd,
                            fd_set *read_fds, int *fdmax){
    if (mesaj->type == EXIT_TYPE){
        // Eliminam si socket-ul din multime.
        FD_CLR (sockfd, read_fds);
        *fdmax = 0;
        close (sockfd);
        return -1;
    }
    else if (mesaj->type == SUB_SUCCESS){
        fprintf (stdout, "Subscribed to topic.\n");
    }
    else if (mesaj->type == UNSUB_SUCCESS){
        fprintf (stdout, "Unsubscribed from topic.\n");
    }
    else if (mesaj->type == TOPIC_TYPE){
        print_message (mesaj);
    }
}

// Functia intoarce 0 daca totul s-a terminat corect, sau -1 in caz contrar
// si se inchide clientul. Daca serverul ne da mesaj sa ne inchidem, atunci
// functia returneaza -1.
int rcv_from_server (int sockfd, fd_set *read_fds, int *fdmax){
    int rcv = 0;
    char *cur_msg;
    memset (buffer, 0, MSG_LEN);
    
    int ret = recv(sockfd, buffer, sizeof(buffer), 0);

    if (ret < 0){
        fprintf (stderr, "A aparut o eroare la recieve."
                " Inchidem conexiunea.\n");

        return -1;
    }

    rcv = ret;

    while (rcv > 0){
        cur_msg = buffer;
        
        // Vrem sa citim pana obtinem cei 2 octeti care ne dau lungimea mesajului.
        while (rcv < 2){
            ret = recv (sockfd, buffer + rcv, sizeof(buffer) - rcv, 0);
            if (ret < 0){
                fprintf (stderr, "A aparut o eroare la recieve."
                        " Inchidem conexiunea.\n");

                exit (2);
            }
            rcv += ret;
        }

        uint16_t len = ntohs (*(uint16_t *) cur_msg);

        while (rcv < len){
            ret = recv (sockfd, buffer + rcv, sizeof (buffer) - rcv, 0);
            if (ret < 0){
                fprintf (stderr, "A aparut o eroare la recieve."
                        " Inchidem conexiunea.\n");

                exit (2);
            }

            rcv += ret;
        }

        while (rcv >= len){
            struct message* mesaj = (struct message*) cur_msg;
            ret = print_type_of_message (mesaj, sockfd, read_fds, fdmax);
            rcv = rcv - len;

            if (rcv < 2){
                break;
            }

            cur_msg = cur_msg + len;
            len = ntohs (*(uint16_t *) cur_msg);
        }

        // Daca s-a citit doar o bucata din mesaj, atunci in golim bufferul
        // astfel incat sa ramana doar mesajul partial, si ne pregatim sa
        // primim continuarea acestuia.
        if (rcv > 0){
            memcpy (buffer, cur_msg, rcv);
            memset (buffer + rcv, 0, sizeof (buffer) - rcv);
        }
    }

    return ret;
}

int read_from_stdin (int sockfd, fd_set *read_fds, int *fdmax){
    memset(buffer, 0, MSG_LEN);
    read(STDIN_FILENO, buffer, MSG_LEN);

    if (strncmp (buffer, "exit", 4) == 0 && strlen (buffer) == 5){
        struct message msg;
        uint16_t len;
        int ret, sent = 0;
        int tries = 3;

        // In payload vom avea id-ul.
        msg.len = htons (sizeof (uint16_t) * 2);
        len = ntohs (msg.len);
        msg.type = EXIT_TYPE;
        memcpy (buffer, (char *)&msg, len);

        send_buffer (sockfd, len, tries);

        FD_CLR (sockfd, read_fds);
        *fdmax = 0;
        close (sockfd);
        return -1;
    }
    else{
        char *p = strtok (buffer, " ");

        if (strcmp (p, "subscribe") != 0 && strcmp (p, "unsubscribe") != 0){
            fprintf (stderr, "Nu este o comanda admisibila.\n");
            return 0;
        }
        else if (strcmp (p, "subscribe") == 0){
            struct message msg;
            char sf;
            p = strtok (NULL, " ");

            if (p == NULL){
                fprintf (stderr, "Nu este o comanda admisibila.\n");
                return 0;
            }

            // Topicurile au maxim 50 de caractere. Nu avem voie unul mai mare.
            if (strlen (p) > 50){
                fprintf (stderr, "Nu este o comanda admisibila.\n");
                return 0;
            }

            strcpy (msg.topic, p);
            p = strtok (NULL, " ");

            if (p == NULL){
                fprintf (stderr, "Nu este o comanda admisibila.\n");
                return 0;
            }

            if (p[strlen(p) - 1] == '\n'){
                p[strlen(p) - 1] = '\0';
            }
            sf = atoi (p);
            if (sf != SF_TRUE && sf != SF_FALSE){
                fprintf (stderr, "Nu este o comanda admisibila.\n");
                return 0;
            }

            msg.len = htons (3 * sizeof (uint16_t) + sizeof (uint32_t)
                             + 51 * sizeof (char));
            msg.type = SUBSCRIBE_TYPE;
            msg.ip = 0;
            msg.port = 0;
            msg.data_type = sf;
            memcpy (buffer, (char *)&msg, ntohs (msg.len));
            send_buffer (sockfd, ntohs (msg.len), 0);
        }
        else{
            p = strtok (NULL, " ");
            if (p == NULL){
                fprintf (stderr, "Nu este o comanda admisibila.\n");
                return 0;
            }

            if (p[strlen(p) - 1] == '\n'){
                p[strlen(p) - 1] = '\0';
            }

            // Topicurile au maxim 50 de caractere. Nu avem voie unul mai mare.
            if (strlen (p) > 50){
                fprintf (stderr, "Nu este o comanda admisibila.\n");
                return 0;
            }

            struct message msg;
            msg.len = htons (3 * sizeof (uint16_t) + sizeof (uint32_t) +
                             strlen (p) * sizeof(char));
            msg.type = UNSUBSCRIBE_TYPE;
            msg.ip = 0;
            msg.port = 0;
            memcpy (msg.topic, p, strlen (p));
            memcpy (buffer, (char *)&msg, ntohs (msg.len));
            send_buffer (sockfd, ntohs (msg.len), 0);
        }
    }

    return 0;
}

int main(int argc, char *argv[]){
    int sockfd;
    int is_open = 1;
    struct sockaddr_in serv_addr;
    fd_set read_fds; // Aceasta va fi multimea de descriptori folositi.
    fd_set temp_fds; // Va fi o multime temporara de descriptori.
    int fdmax = 0;

    FD_ZERO (&read_fds);
    FD_ZERO (&temp_fds);
    FD_SET (0, &read_fds); // Introducem descriptorul pentru stdin.

    if (argc < 4){
        fprintf (stderr, "Nu s-a apleat corect deschiderea subscriber-ului.\n");
        fprintf (stderr, "Utilizare: ./subscriber <ID_CLIENT>"
                " <IP_SERVER> <PORT_SERVER>\n");
        exit (1);
    }

    setvbuf (stdout, NULL, _IONBF, BUFSIZ);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0){
        fprintf (stderr, "Nu s-a putut crea socket-ul. Inchidem conexiunea.\n");
        exit(2);
    }

    // Ne introducem socketul in multime.
    FD_SET (sockfd, &read_fds);
    fdmax = sockfd;

    // Populam adresa.
    memset (&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(atoi(argv[3]));
	int ret = inet_aton(argv[2], &serv_addr.sin_addr);

    if (ret < 0){
        fprintf (stderr, "Au aparut probleme la crearea socket-ului."
                " Inchidem conexiunea.\n");
        exit (2);
    }

    // Dezactivam algoritmul lui Nagle. Daca esueaza nu oprim totusi conexiunea
    int flag = 1;
    ret = setsockopt (sockfd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
    if (ret < 0){
        fprintf (stderr, "NU s-a putut dezactiva Nagle.\n"); 
    }

    // Incercam sa ne conectam la server.
    ret = connect(sockfd, (struct sockaddr*) &serv_addr, sizeof(serv_addr));
    if (ret < 0){
        fprintf (stderr, "Nu s-a putut conecta la server."
               " Inchidem conexiunea.\n");
    }

    // Dupa ce s-a reusit conectarea, trimitem ID-ul clientului.
    send_name (sockfd, argv[1]);

    while (is_open){
        for (int i = 0; i <= fdmax; i++){
            temp_fds = read_fds;

            ret = select (fdmax + 1, &temp_fds, NULL, NULL, NULL);
            if (ret < 0){
                fprintf (stderr, "A aparut o problema la select.\n");
                exit (2);
            }

            if (FD_ISSET (i, &temp_fds)){
                if (i == sockfd){
                    ret = rcv_from_server (sockfd, &read_fds, &fdmax);
                    
                    // Daca ret este -1 inseamna ca server-ul ne-a anuntat
                    // sa ne inchidem.
                    if (ret == -1){
                        is_open = 0;
                    }
                }
                else if (i == 0){
                    ret = read_from_stdin (sockfd, &read_fds, &fdmax);

                    // Daca se returneaza -1, inseamna ca s-a primit exit de la
                    // tastatura.
                    if (ret == -1){
                        is_open = 0;
                    }
                }
            }
        }
    }
}