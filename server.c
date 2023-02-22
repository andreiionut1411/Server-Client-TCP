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
#define DGRAM_LEN 1551 // 50 + 1 + 1500 = 1551
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
int sent = 0;

// Aceasta este structura de date prin care vom transmite mesajele.
struct message{
    uint16_t len;
    uint16_t type; // E uint16 pentru a evita padding-ul automat facut de C.
    uint32_t ip; // Va fi nevoie pentru a transmite IP-ul clientului UDP.
    uint16_t port; 
    char topic[50];
    char data_type;
    char payload[PAYLOAD_LEN];
};

// Ne va ajuta sa tinem mesajele care trebuiesc transmise ca o coada.
struct message_list{
    struct message *mesaj;
    struct message_list *next;
};

// Vom tine clientii tcp intr-o lista simplu inlantuita.
struct conexiune_tcp{
    char *client_id;
    int sockfd;
    struct sockaddr_in client_addr;
    struct hash_entry_list* subscriptions; // Aici tinem o lista cu abonamentele.
    struct conexiune_tcp* next;
};

// Campul mesaje netrimise va fi NULL, pana cand un subscriber care avea
// optiunea de store-and-forward activa se va conecta. In acest caz, ii
// copiem numele in client_id si tinem minte mesajele pana cand se intoarce.
// Cand se intoarce, trimitem toate mesajele si golim si campul de client_id.
struct subscriber{
    struct conexiune_tcp* client;
    uint8_t sf;
    struct message_list* mesaje_netrimise;
    struct subscriber* next;
};

// Vom avea o tabela hash in care tinem topic-urile. Daca 2 topic-uri au aceiasi
// valoare hash, atunci le tinem intr-o lista simplu inlantuita. Fiecare topic
// va avea la randul sau o lista cu clientii abonati la acel topic.
// Daca un client vrea sa se deazaboneze, atunci pur si simplu il eliminam din
// lista.
struct hash_entry{
    char topic[50];
    struct hash_entry* next_topic;
    struct subscriber* subscribers;
};

struct hash_entry_list{
    struct hash_entry* cur_entry;
    struct hash_entry_list* next;
};

char buffer[MSG_LEN];

struct conexiune_tcp *clienti_tcp;
struct conexiune_tcp *last_client;
struct hash_entry *hash_table[1000];

// Facem o functie hash pentru a putea distribui topic-urile in tabela hash.
int hash_function (char *nume){
    int len = strlen (nume);
    int hash = 7;

    for (int i = 0; i < len; i++){
        hash = hash * 17 + nume[i];
        hash = hash % 1000;
    }

    return hash;
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

void send_topic (char *topic, struct message* msg){
    int hash = hash_function (topic);
    struct hash_entry* entry = hash_table[hash];

    if (entry == NULL){
        return;
    }

    memcpy (buffer, msg, ntohs (msg->len));

    while (entry != NULL){
        if (strcmp (entry->topic, topic) == 0){
            break;
        }
        entry = entry->next_topic;
    }

    // Daca nu exista niciun abonat la topic, nu trimitem mesajul.
    if (entry == NULL){
        return;
    }

    struct subscriber* sub = entry->subscribers;

    while (sub != NULL){
        if (sub->client->sockfd != 0){
            send_buffer (sub->client->sockfd, ntohs (msg->len), 0);
        }
        // Daca clientul e deconectat dar avea SF activ, atunci ii retinem
        // mesajele.
        else if (sub->sf == SF_TRUE){
            struct message_list* new_message = malloc (sizeof (struct message_list));
            if (new_message == NULL){
                fprintf (stderr, "Nu s-a putut retine mesajul cu"
                        " store-and-fowrward pentru un client.\n");
                continue;
            }

            // Adaugam mesajul in lista de mesaje netrimise.
            new_message->next = sub->mesaje_netrimise;
            sub->mesaje_netrimise = new_message;
            
            struct message* mesaj = malloc (sizeof (struct message));
            if (mesaj == NULL){
                fprintf (stderr, "Nu s-a putut retine mesajul cu"
                        " store-and-fowrward pentru un client.\n");
                continue;
            }
            int len_payload = ntohs (msg->len) - 3 * sizeof (uint16_t) -
                              sizeof (uint32_t) - 51 * sizeof (char);
            mesaj->len = msg->len;
            mesaj->type = msg->type;
            mesaj->ip = msg->ip;
            mesaj->port = msg->port;
            memcpy (mesaj->topic, msg->topic, 50);
            mesaj->data_type = msg->data_type;
            memcpy (mesaj->payload, msg->payload, len_payload);
            new_message->mesaj = mesaj;
        }

        sub = sub->next;
    }
}

void udp_message (uint32_t ip, uint16_t port, char* payload){
    struct message msg;
    int len = 0; // lungimea tipului de date

    msg.type = TOPIC_TYPE;
    msg.ip = ip;
    msg.port = port;
    strncpy (msg.topic, payload, 50);
    msg.data_type = *(payload + 50);

    if (msg.data_type == INT){
        len = sizeof (uint8_t) + sizeof (uint32_t);
    }
    else if (msg.data_type == SHORT_REAL){
        len = sizeof (uint16_t);
    }
    else if (msg.data_type == FLOAT){
        len = 2 * sizeof (uint8_t) + sizeof (uint32_t);
    }
    else if (msg.data_type == STRING){
        len = strlen (payload + 51) + 1;
    }
    else{
        fprintf (stderr, "Am primit un mesaj gresit de la clientul UDP, nu il"
                " trimitem mai departe.\n");

        return;
    }

    msg.len = htons (3 * sizeof (uint16_t) + sizeof (uint32_t) + 
                     51 * sizeof(char) + len);
    memcpy (msg.payload, payload + 51, len);

    if (msg.data_type == STRING){
        msg.payload[len - 1] = '\0';
    }

    send_topic (msg.topic, &msg);
}

// Functia elimina din multime socket-ul primit ca parametru.
void eliminate_socket (int sockfd, fd_set *read_fds, int *fdmax){

    // Nu eliminam socketul pentru stdin.
    if (sockfd == 0){
        return;
    }

    FD_CLR (sockfd, read_fds);

    // Verificam daca am eliminat socket-ul maxim.
    if (*fdmax == sockfd){
        int new_fdmax = 0;
        for (int i = 0; i < *fdmax; i++){
            if (FD_ISSET (i, read_fds)){
                new_fdmax = i;
            }
        }

        *fdmax = new_fdmax;
    }
}

// Functia primeste un client si il elimina din lista de clienti. Aceasta
// functie este folosita pentru a elimina definitiv un client in situatiile
// in care se incearca conectarea a 2 clienti cu acelasi ID. Mai este
// utila si atunci cand se reintoarce un client deoarece pentru acesta, mai
// intai am introdus un client fara nume in lista care trebuie acum eliminat.
void eliminate_client (int sockfd){
    struct conexiune_tcp* cur_client = clienti_tcp;
    struct conexiune_tcp* prev_client = NULL;

    while (cur_client != NULL){
        if (cur_client->sockfd == sockfd){
            break;
        }
        else{
            prev_client = cur_client;
            cur_client = cur_client->next;
        }
    }

    // Nu ar trebui sa se intre vreodata in acest if.
    if (cur_client == NULL){
        fprintf (stderr, "A aparut o problema la inchiderea unui client.\n");
        return;
    }

    if (prev_client == NULL){
        clienti_tcp = cur_client->next;
    }
    else{
        prev_client->next = cur_client->next;
    }

    if (cur_client->next == NULL){
        last_client = prev_client;
    }

    if (cur_client->client_id != NULL){
        free (cur_client->client_id);
    }

    free (cur_client);
}

// Functia inchide socketul pentru un client. Daca clientul trebuie sa
// primeasca un mesaj ca sa se inchida si el, atunci to_client va fi setat
// ca 1, altfel va fi 0. Daca delete este 1, atunci clientul nu numai ca va
// fi inchis, dar va fi si eliminat din lista de clienti.
int close_client (int sockfd, fd_set *read_fds, int *fdmax,
                  int to_client, int delete){

    struct conexiune_tcp* cur_client = clienti_tcp;
    int found = 0;

    while (found == 0 && cur_client != NULL){
        if (cur_client->sockfd == sockfd){
            found = 1;
        }
        else{
            cur_client = cur_client->next;
        }
    }

    // Daca nu am gasit socket-ul inseamna ca el a fost inchis deja.
    if (found == 0){
        return -1;
    }
    else{
        // Daca clientul a fost cel care a initiat inchiderea, atunci afisam
        // acest lucru.
        if (to_client == 0){
            fprintf (stdout, "Client ");
            fprintf (stdout, "%s", cur_client->client_id);
            fprintf (stdout, " disconnected.\n");
        }

        eliminate_socket (cur_client->sockfd, read_fds, fdmax);

        // Daca clientul se inchide, il pastram totusi in lista, in caz ca se
        // intoarce mai tarziu si trebuie sa ramana abonat la topicurile la care
        // a dat subscribe. Pentru a stii ca un client nu mai este activ, acesta
        // va avea sockfd == 0, pentru ca 0 este rezervat pentru stdin, si
        // niciun socket activ nu poate avea aceasta valoare.
        if (delete == 0){
            cur_client->sockfd = 0;
            cur_client->client_addr.sin_port = 0;
            cur_client->client_addr.sin_addr.s_addr = 0;
        }
        else{
            eliminate_client (sockfd);
        }
    }

    // Daca trebuie, ii trimitem un mesaj clientului sa se inchida.
    if (to_client == 1){
        // Mesajul va avea doar type si lungime, restul campurilor 
        // nu ne intereseaza.
        struct message exit_message;
        exit_message.type = EXIT_TYPE;
        exit_message.len = htons (sizeof (uint16_t) + 1 * sizeof (char));
        int len = ntohs (exit_message.len);

        memcpy (buffer, (char *)&exit_message, len);

        send_buffer (sockfd, len, 0);
    }

    close (sockfd);

    return 0;
}

// Functia inchide toti socketii pe care se afla clienti TCP si le trimite
// acestora mesajul de a se inchide si ei.
void close_server (fd_set *read_fds, int *fdmax){
    // Mesajul va avea doar type si lungime, restul campurilor
    // nu ne intereseaza.
    struct message exit_message;
    exit_message.type = EXIT_TYPE;
    exit_message.len = htons (sizeof (uint16_t) + 1 * sizeof (char));
    int len = ntohs (exit_message.len);

    memcpy (buffer, (char *)&exit_message, len);
    struct conexiune_tcp* cur_client = clienti_tcp;

    while (cur_client != NULL){
        int tries = 3;

        // Pentru fiecare client inchidem socketul, asta daca nu cumva este 0,
        // ceea ce inseamna ca clientul este deconectat si a ramas in asteptare
        // pentru reconectarea sa.
        if (cur_client->sockfd != 0){
            send_buffer (cur_client->sockfd, len, tries);

            close (cur_client->sockfd);

            if (cur_client->client_id != NULL)
                free (cur_client->client_id);
        }

        struct hash_entry_list* subscriptions = cur_client->subscriptions;
        while (subscriptions != NULL){
            struct hash_entry_list* next_subscription = subscriptions->next;
            free (subscriptions);
            subscriptions = next_subscription;
        }

        struct conexiune_tcp* aux = cur_client;
        cur_client = cur_client->next;
        free (aux);
    }

    // Golim tabela hash si tot ce se afla in ea.
    for (int i = 0; i < 1000; i++){
        struct hash_entry* entry = hash_table[i];

        while(entry != NULL){
            struct subscriber* subscriber = entry->subscribers;

            while (subscriber != NULL){
                struct message_list* mesaje_netrimise = 
                                     subscriber->mesaje_netrimise;

                while (mesaje_netrimise != NULL){
                    struct message_list* next_mesaj = mesaje_netrimise->next;
                    free (mesaje_netrimise);
                    mesaje_netrimise = next_mesaj;
                }

                struct subscriber* next_sub = subscriber->next;
                free (subscriber);
                subscriber = next_sub;
            }

            struct hash_entry* next_entry = entry->next_topic;
            free (entry->topic);
            free (entry);
            entry = next_entry;
        }
    }
}

// Functia trimite toate mesajele care au fost stocate cat timp a fost clientul
// deconectat.
void send_unsent_messages (struct conexiune_tcp* client){
    struct hash_entry_list* subscriptions = client->subscriptions;

    // Pentru fiecare abonament cautam printre subscriberi pana gasim clientul
    // nostru, si daca SF este 1, atunci trimitem mesajele, atlfel trecem
    // la urmatorul abonament.
    while (subscriptions != NULL){
        struct subscriber* subscriber = subscriptions->cur_entry->subscribers;

        while (subscriber != NULL){
            if (strcmp (subscriber->client->client_id, client->client_id) == 0){
                break;
            }

            subscriber = subscriber->next;
        }

        // NU ar trebui sa se intre vreodata pe acest if.
        if (subscriber == NULL){
            fprintf (stderr, "A aparut o eroare la pastrarea subscriberilor"
                    " in tabela hash.\n");
            return;
        }

        // Daca SF este 1, atunci trimitem toate mesajele.
        if (subscriber->sf == SF_TRUE){
            struct message_list* mesaje_netrimise = subscriber->mesaje_netrimise;

            while (mesaje_netrimise != NULL){
                memset (buffer, 0, MSG_LEN);
                memcpy (buffer, (char *)mesaje_netrimise->mesaj,
                        ntohs (mesaje_netrimise->mesaj->len));
                send_buffer (client->sockfd,
                             ntohs (mesaje_netrimise->mesaj->len), 0);

                struct message_list* next_msg = mesaje_netrimise->next;
                free (mesaje_netrimise->mesaj);
                free (mesaje_netrimise);
                mesaje_netrimise = next_msg;
            }

            subscriber->mesaje_netrimise = NULL;
        }

        subscriptions = subscriptions->next;
    }
}

// Functia creaza un nou element in lista de conexiuni tcp, atunci cand se
// conecteaza un socket nou. Acest element nu are ID, acesta urmand sa fie
// transmis de client ca primul sau mesaj. Daca noul socket a fost deschis
// de un client care are un ID care exista deja, sau care a mai fost conectat
// si in trecut, atunci trebuie sa se elimine din lista conexiunea nou creata.
void tcp_connection(int tcp_sockfd, fd_set *read_fds, int *fdmax){
    int newsockfd;
    struct sockaddr_in client_addr;
    socklen_t clen = sizeof(client_addr);

    newsockfd = accept (tcp_sockfd, (struct sockaddr *) &client_addr, &clen);
    if (newsockfd < 0){
        fprintf (stderr, "Nu s-a putut accepta conexiunea.\n");
    }

    // Dezactivam algoritmul lui Nagle. Daca esueaza nu oprim totusi conexiunea
    int flag = 1;
    int ret = setsockopt (newsockfd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag,
                          sizeof(int));
    if (ret < 0){
        fprintf (stderr, "NU s-a putut dezactiva Nagle.\n"); 
    }

    // Introducem noul socket in multime.
    FD_SET (newsockfd, read_fds);
    if (newsockfd > *fdmax){
        *fdmax = newsockfd;
    }

    // Adaugam clientul TCP nou in vectorul nostru de clienti.
    struct conexiune_tcp *client = malloc (sizeof (struct conexiune_tcp));

    // Daca nu s-a reusit alocarea, atunci inchidem conexiunea.
    if (client == NULL){
        fprintf (stderr, "A aparut o eroare la crearea clientului.\n");
    }
    client->sockfd = newsockfd;
    client->client_id = NULL;
    client->client_addr = client_addr;
    client->subscriptions = NULL;
    client->next = NULL;
    
    if (clienti_tcp == NULL){
        clienti_tcp = client;
        last_client = client;
    }
    else{
        last_client->next = client;
        last_client = client;
    }
}

int rcv_name_of_client (int sockfd, struct message* mesaj, fd_set *read_fds,
                        int *fdmax){;
    struct conexiune_tcp* cur_client = clienti_tcp;
    struct conexiune_tcp* prev_client = NULL;
    struct conexiune_tcp* searched_client = NULL;
    struct conexiune_tcp* ex_client = NULL;
    int ok = 1;

    // Verificam daca exista deja un client cu acelasi ID.
    while (cur_client != NULL){
        if (cur_client->client_id != NULL){
            if (strcmp (cur_client->client_id, mesaj->payload) == 0){

                // Verificam daca nu s-a intors un fost client.
                if (cur_client->sockfd == 0){
                    ex_client = cur_client;
                }
                else{
                    ok = 0;
                }
            }
        }
        
        if (cur_client->sockfd == sockfd){
            searched_client = cur_client;
        }
        else{
            prev_client = cur_client;
        }

        cur_client = cur_client->next;
    }

    // Daca ID-ul nu exista atunci ii asociam socketului ID-ul.
    if (ok == 1 && searched_client != NULL && ex_client == NULL){
        if (searched_client->client_id == NULL){
            searched_client->client_id = malloc (strlen (mesaj->payload));
            if (searched_client->client_id == NULL){
                fprintf (stderr, "A aparut o problema la asocierea clientului"
                        " cu numele acestuia. Inchidem conexiunea.\n");

                close_server(read_fds, fdmax);
                exit (3);
            }

            strcpy (searched_client->client_id, mesaj->payload);

            // Afisam confirmarea ca s-a conectat clientul.
            fprintf (stdout, "New client ");
            fprintf (stdout, "%s", searched_client->client_id);
            fprintf (stdout, " connected from ");
            fprintf (stdout, "%s", inet_ntoa 
                            (searched_client->client_addr.sin_addr));
            fprintf (stdout, ":%d.\n", ntohs 
                            (searched_client->client_addr.sin_port));

            return 0;
        }
    }
    // Daca ID-ul exista, eliminam socketul si clientul din lista.
    else if (searched_client != NULL && ex_client == NULL){
        fprintf (stdout, "Client ");
        fprintf (stdout, "%s", mesaj->payload);
        fprintf (stdout, " already connected.\n");
        close_client (sockfd, read_fds, fdmax, 1, 1);
    }
    else if (ex_client != NULL){
        // S-a intors un fost client.
        ex_client->client_addr = searched_client->client_addr;

        // Doar eliminam din lista clientul tcp creat de tcp_connection.
        eliminate_client (sockfd);
        ex_client->sockfd = searched_client->sockfd;

        // Afisam confirmarea ca s-a reconectat clientul.
        fprintf (stdout, "New client ");
        fprintf (stdout, "%s", ex_client->client_id);
        fprintf (stdout, " connected from ");
        fprintf (stdout, "%s", inet_ntoa (ex_client->client_addr.sin_addr));
        fprintf (stdout, ":%d.\n", ntohs (ex_client->client_addr.sin_port));

        send_unsent_messages (ex_client);
    }

    // Se intoarce -1 daca a aparut o eroare si socket-ul cautat nu exista.
    return -1;
}

// Functia inroleaza un client la un abonament. Daca apar probleme de alocare
// a memoriei sau este deja abonat, atunci se intoarce -1. Altfel, se
// returneaza 0. Daca se returneaza 0, atunci trebuie sa ii trimitem clientului
// un mesaj de acceptare a abonamentului.
int subscribe_client (struct conexiune_tcp* client, char *topic, int sf){
    int hash = hash_function (topic);
    struct hash_entry *entry = hash_table[hash];
    char *cur_topic = NULL;

    if (entry != NULL){
        cur_topic = entry->topic;
    }

    while (entry != NULL && strcmp (cur_topic, topic) != 0){
        entry = entry->next_topic;
        cur_topic = entry->topic;
    }

    // Daca nu exista inca topic-ul, il cream.
    if (entry == NULL){
        entry = malloc (sizeof (struct hash_entry));
        if (entry == NULL){
            fprintf (stderr, "Nu s-a putut crea topicul.\n");
            return -1;
        }

        strcpy (entry->topic, topic);
        entry->next_topic = hash_table[hash];
        hash_table[hash] = entry; // Adaugam topicul la inceputul listei.
        entry->subscribers = NULL;
    }

    struct subscriber* cur_sub = entry->subscribers;

    // Verificam daca nu cumva clientul era deja subscriber la topic.
    while (cur_sub != NULL){
        if (strcmp (cur_sub->client->client_id, client->client_id) == 0){
            fprintf (stderr, "Clientul era deja subscriber la topic.\n");
            return -1;
        }
        cur_sub = cur_sub->next;
    }

    struct subscriber* new_sub = malloc (sizeof (struct subscriber));
    if (new_sub == NULL){
        fprintf (stderr, "A aparut o problema la alocare.\n");
        return -1;
    }

    new_sub->client = client;
    new_sub->sf = sf;
    new_sub->mesaje_netrimise = NULL;
    new_sub->next = entry->subscribers;
    entry->subscribers = new_sub; // Am adaugat noul abonat primul in lista.

    struct hash_entry_list* subscriptions_client = malloc (sizeof 
                                                    (struct hash_entry_list));
    if (subscriptions_client == NULL){
        fprintf (stderr, "A aparut o problema la alocare.\n");
        return -1;
    }

    // Adaugam in lista de abonamente a clientului noul subscribe.
    subscriptions_client->cur_entry = entry;
    subscriptions_client->next = client->subscriptions;
    client->subscriptions = subscriptions_client;

    return 0;
}

// Dupa ce am primit cererea de subscribe, confirmam daca aceasta s-a facut
// cu succes. Daca abonatul dorea sa se aboneze la un topic la care deja
// era abonat, atunci serverul va afisa mesajul de eroare, iar clientul
// nu va primi confirmarea.
void send_sub_confirmation (int sockfd){
    struct message msg;
    msg.len = htons (sizeof (uint16_t) * 2);
    msg.type = SUB_SUCCESS;

    memcpy (buffer, (char *)&msg, ntohs(msg.len));
    send_buffer (sockfd, ntohs (msg.len), 0);
}

// Functia dezaboneaza un client de la un topic, daca acesta este deja abonat
// si daca topicul exista. Daca se face unsubscribe corect, atunci se
// returneaza 0 si se trimite o confirmare. Daca nu, atunci serverul
// afiseaza un mesaj de eroare.
int unsubscribe_client (struct conexiune_tcp* client, char *topic){
    struct hash_entry_list* abonament = client->subscriptions;
    struct hash_entry_list* prev_ab = NULL;
    struct hash_entry* entry;
    char *cur_topic;

    if (abonament == NULL){
        fprintf (stderr, "Topic-ul de la care se doreste sa se faca"
                 " unsubscribe nu exista.\n");
        return -1;
    }

    while (abonament != NULL){
        if (strcmp (abonament->cur_entry->topic, topic) != 0){
            prev_ab = abonament;
            abonament = abonament->next;
        }
        else{
            break;
        }
    }

    if (abonament == NULL){
        fprintf (stderr, "Topic-ul de la care se doreste sa se faca"
                 " unsubscribe nu exista.\n");
        return -1;
    }

    // Eliminam abonamentul din lista clientului, iar apoi eliminam clientul
    // din lista abonamentului din tabela hash.
    if (prev_ab == NULL){
        client->subscriptions = abonament->next;
    }
    else{
        prev_ab->next = abonament->next;
    }

    entry = abonament->cur_entry;

    struct subscriber* cur_sub = entry->subscribers;
    struct subscriber* prev_sub = NULL;

    while (cur_sub != NULL){
        if (cur_sub->client->sockfd != client->sockfd){
            prev_sub = cur_sub;
            cur_sub = cur_sub->next;
        }
        else{
            break;
        }
    }

    // Verificam daca am gasit clientul printre subscriberi.
    if (cur_sub == NULL){
        fprintf (stderr, "Clientul a incercat sa se dezaboneze de la un topic"
                 " la care nu era subscriber.\n");
        return -1;
    }

    if (prev_sub == NULL){
        entry->subscribers = cur_sub->next;
    }
    else{
        prev_sub->next = cur_sub->next;
    }

    // Daca a dat unsubscribe ultimul client, atunci eliminam topicul din tabela.
    if (entry->subscribers == NULL){
        int hash = hash_function (topic);
        struct hash_entry* cur_entry = hash_table[hash];
        struct hash_entry* prev_entry = NULL;
        cur_topic = cur_entry->topic; // Nu e NULL pentru ca stim ca exista topicul.

        while (cur_entry != NULL){
            if (strcmp (cur_topic, topic) != 0){
                prev_entry = cur_entry;
                cur_entry = cur_entry->next_topic;
                cur_topic = cur_entry->topic;
            }
            else{
                break;
            }
        }

        // Nu ar trebui sa se intre niciodata pe acest if.
        if (cur_entry == NULL){
            fprintf (stderr, "A aparut o eroare in tabela hash.\n");
            return -1;
        }

        if (prev_entry == NULL){
            hash_table[hash] = cur_entry->next_topic;
        }
        else{
            prev_entry->next_topic = cur_entry->next_topic;
        }

        free (cur_entry->topic);
        free (cur_entry);
    }

    free (cur_sub);
    free (abonament);
    return 0;
}

// Daca s-a facut unsubscribe cu succes, atunci informam clientul.
void send_unsub_confirmation (int sockfd){
    memset (buffer, 0, MSG_LEN);
    struct message confirmation;

    confirmation.len = htons (sizeof (uint16_t) * 2);
    confirmation.type = UNSUB_SUCCESS;

    memcpy (buffer, (char *)&confirmation, ntohs (confirmation.len));
    send_buffer (sockfd, ntohs (confirmation.len), 0);
}

// Functia primeste ca parametru un mesaj si in functie de tipul mesajului
// se face operatia corespunzatoare.
void interpret_message (struct message* mesaj, int sockfd, fd_set *read_fds,
                        int *fdmax){
    
    int ret;

    if (mesaj->type == NAME_TYPE){
        rcv_name_of_client (sockfd, mesaj, read_fds, fdmax);
    }
    else if (mesaj->type == EXIT_TYPE){
        close_client (sockfd, read_fds, fdmax, 0, 0); // Nu trimitem mesaj inapoi.
    }
    else if (mesaj->type == SUBSCRIBE_TYPE){
        struct conexiune_tcp *client = clienti_tcp;
        while (client != NULL && client->sockfd != sockfd){
            client = client->next;
        }

        // Daca clientul nu exista atunci inseamna ca a aparut o eroare.
        // In mod normal, nu ar trebui sa intre niciodata in acest if.
        if (client == NULL){
            fprintf (stderr, "A aparut o eroare la socketi.\n");
            close (sockfd);
            FD_CLR (sockfd, read_fds);
        }
        int sf = mesaj->data_type;
        mesaj->data_type = '\0';
        ret = subscribe_client (client, mesaj->topic, sf);
        if (ret == 0){
            send_sub_confirmation (sockfd);
        }
    }
    else if (mesaj->type == UNSUBSCRIBE_TYPE){
        struct conexiune_tcp *client = clienti_tcp;
        while (client != NULL && client->sockfd != sockfd){
            client = client->next;
        }


        // Daca clientul nu exista atunci inseamna ca a aparut o eroare.
        // In mod normal, nu ar trebui sa intre niciodata in acest if.
        if (client == NULL){
            fprintf (stderr, "A aparut o eroare la socketi.\n");
            close (sockfd);
            FD_CLR (sockfd, read_fds);
        }

        ret = unsubscribe_client (client, mesaj->topic);
        if (ret == 0){
            send_unsub_confirmation (sockfd);
        }
    }
}

void rcv_from_tcp (int sockfd, fd_set *read_fds, int *fdmax){
    memset (buffer, 0, MSG_LEN);
    int ret;
    int rcv, len;
    char *cur_msg;
    struct message* mesaj;

    rcv = recv (sockfd, buffer, sizeof (buffer), 0);
    if (rcv < 0){
        fprintf (stderr, "A fost o eroare in conexiune."
               " Inchidem conexiunea.\n");
        close_server(read_fds, fdmax);
        exit (2);
    }
    
    // Daca nu mai primim nimic pe socket, atunci inseamna ca s-a terminat
    // conexiunea fara sa ne fii anuntat si stergem socket-ul.
    if (rcv == 0){
        close_client (sockfd, read_fds, fdmax, 0, 0);

        return;
    }

    while (rcv > 0){
        cur_msg = buffer;

        // Pana primim primele 2 caractere care ne dau lungimea totala nu ne oprim.
        while (rcv < 2){
            ret = recv (sockfd, buffer + rcv, sizeof(buffer), 0);
            rcv += ret;
            if (rcv < 0){
                fprintf (stderr, "A fost o eroare in conexiune."
                    " Inchidem conexiunea.\n");
                close_server (read_fds, fdmax);
                exit (2);
            }
        }

        len = ntohs (*(uint16_t *)buffer);
        
        // Pana nu se termina mesajul nu ne oprim din primit.
        while (rcv < len){
            ret = recv (sockfd, buffer + rcv, sizeof(buffer), 0);
            rcv += ret;
            if (rcv < 0){
                fprintf (stderr, "A fost o eroare in conexiune."
                    " Inchidem conexiunea.\n");
                close_server (read_fds, fdmax);
                exit (2);
            }
        }

        // Daca am primit in buffer mai mult de un mesaj, atunci le analizam pe
        // fiecare in parte.
        while (rcv >= len){
            mesaj = (struct message*) cur_msg;
            interpret_message (mesaj, sockfd, read_fds, fdmax);
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
}

// Functia returneaza 0 daca s-a primit comanda de exit, sau 0 in caz contrar.
int read_from_stdin (fd_set *read_fds, int *fdmax){
    memset(buffer, 0, MSG_LEN);
    read(0, buffer, MSG_LEN);

    // Daca nu primim exit de la tastatura, anuntam user-ul, iar apoi pur
    // si simplu continuam sa asteptam alte comenzi.
    if (strlen (buffer) > 5 || strncmp (buffer, "exit", 4) != 0){
        fprintf (stderr, "Server-ul primeste doar exit.\n");
        return -1;
    }

    // Daca se ajunge aici inseamna ca s-a dat comanda de exit.
    close_server (read_fds, fdmax);
    return 0;
}

void start_sockets (int *udp_sockfd, int *tcp_sockfd, fd_set *read_fds,
                    int *fdmax, char* argv[]){
    struct sockaddr_in servaddr;

    // Ne pornim un socket pentru conexiunile UDP.
    if (((*udp_sockfd) = socket (AF_INET, SOCK_DGRAM, 0)) < 0){
        fprintf (stderr, "Nu s-a putut crea socket-ul. Inchidem conexiunea.\n");
        close_server (read_fds, fdmax);
        exit(2);
    }

    // Completam informatiile pentru servaddr.
    memset (&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET; // Familia de IPv4
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(atoi(argv[1]));

    int rc = bind(*udp_sockfd, (struct sockaddr *)&servaddr, 
                  sizeof(struct sockaddr_in));
    if (rc < 0){
        fprintf (stderr, "Probleme la facut bind-ul. Inchidem conexiunea.\n");
        close_server (read_fds, fdmax);
        exit (2);
    }

    // Adaugam descriptorul pentru noul socket deschis.
    if ((*fdmax) < (*udp_sockfd)){
        *fdmax = *udp_sockfd;
    }
    FD_SET ((*udp_sockfd), read_fds);

    // Cream socket-ul pe care vom asculta pentru conexiuni tcp.
    if (((*tcp_sockfd) = socket (AF_INET, SOCK_STREAM, 0)) < 0){
        fprintf (stderr, "Nu s-a putut crea socket-ul. Inchidem conexiunea.\n");
        close_server (read_fds, fdmax);
        exit (2);
    }
    int ret = bind (*tcp_sockfd, (struct sockaddr*) &servaddr, sizeof 
                    (struct sockaddr_in));
    if (ret < 0){
        fprintf (stderr, "Probleme la facut bind-ul. Inchidem conexiunea.\n");
        close_server (read_fds, fdmax);
        exit (2);
    }

    int flag;
    ret = setsockopt (*tcp_sockfd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag,
                      sizeof(int));
    if (ret < 0){
        fprintf (stderr, "NU s-a putut dezactiva Nagle.\n");
    }

    // Numarul maxim de conexiuni care pot fi in coada este SOMAXCONN, iar
    // orice numar mai mare ca fi trunchiat oricum la acesta.
    ret = listen ((*tcp_sockfd), SOMAXCONN);
    if (ret < 0){
        fprintf (stderr, "Probleme la facut listen. Inchidem conexiunea.\n");
        close_server (read_fds, fdmax);
        exit (2);
    }

    // Adaugam descriptorul pentru socketul pe care vom asculta pentru conexiuni
    // noi pentru TCP.
    if (*fdmax < *tcp_sockfd){
        *fdmax = *tcp_sockfd;
    }
    FD_SET (*tcp_sockfd, read_fds);
}

int main(int argc, char*argv[]){
    int udp_sockfd, tcp_sockfd;
    int newtcp_sockfd, fdmax = 0;
    int is_open = 1, ret;
    fd_set read_fds; // Aici vom avea descriptorii activi.
    fd_set temp_fds; // Va fi o multime temporara, pentru select.

    setvbuf (stdout, NULL, _IONBF, BUFSIZ);

    if (argc < 2){
        fprintf (stderr, "Nu s-a apleat corect deschiderea server-ului.\n");
        fprintf (stderr, "Utilizare: ./server <PORT_DORIT>\n");
        close_server (&read_fds, &fdmax);
        exit (1);
    }

    clienti_tcp = NULL;
    last_client = NULL;
    for (int i = 0; i < 1000; i++){
        hash_table[i] = NULL;
        // Am initializat o tabela hash goala.
    }

    FD_ZERO (&read_fds);
    FD_ZERO (&temp_fds);
    FD_SET (0, &read_fds); // Introducem descriptorul pentru stdin.

    start_sockets (&udp_sockfd, &tcp_sockfd, &read_fds, &fdmax, argv);
    
    while (is_open){
        struct sockaddr_in client_addr;
        socklen_t clen = sizeof(client_addr);
        temp_fds = read_fds;

        ret = select (fdmax + 1, &temp_fds, NULL, NULL, NULL);
        if (ret < 0){
            fprintf (stderr, "A aparut o problema la select.\n");
            close_server (&read_fds, &fdmax);
            exit (2);
        }

        for (int i = 0; i <= fdmax; i++){
            if (FD_ISSET (i, &temp_fds)){
                if (i == udp_sockfd){
                    memset (buffer, 0, MSG_LEN);
                    int rcv = recvfrom (udp_sockfd, buffer, DGRAM_LEN, 0,
                             (struct sockaddr*)&client_addr, &clen);
                    if (rcv < 0){
                        fprintf (stderr, "A aparut o problema la receive din UDP.\n");
                    }

                    if (rcv != 0){
                        udp_message (client_addr.sin_addr.s_addr, 
                                     client_addr.sin_port, buffer);
                    }
                }
                else if (i == 0){
                    ret = read_from_stdin (&read_fds, &fdmax);

                    // Daca s-a dat comanda de exit, atunci inchidem server-ul.
                    if (ret == 0){
                        is_open = 0;
                        close (tcp_sockfd);
                        break;
                    }
                }
                else if (i == tcp_sockfd){
                    tcp_connection(tcp_sockfd, &read_fds, &fdmax);
                }
                else{
                    rcv_from_tcp (i, &read_fds, &fdmax);
                }
            }
        }
    }

    close(udp_sockfd);
}