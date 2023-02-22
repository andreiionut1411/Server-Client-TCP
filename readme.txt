/* Ionescu Andrei Ionut - 321CB */

								Tema 2

	Modul de functionare a transmisiilor:
	Toti clientii TCP sunt tinuti intr-o lista de conexiuni TCP, structura contine urmatoarele
date despre client: ID, socket, adresa clientului, abonamente, adresa urmatorului
client din lista.
	Pentru aceasta tema am folosit mai multe structuri de date pentru o implementare
rapida. Pentru a tine in memorie topicurile am folosit o tabel hash. Cand un client TCP
face subscribe la un topic, se aplica functia de hash asupra numelui topicului si ne ducem
in tabela hash si cautam topicul in bucketul corespunzator. Daca gasim topicul, atunci
adaugam in lista de clienti ai topicului pe clientul curent. Astfel, fiecare topic stie ce
clienti doresc informatii despre acesta. Daca nu gasim topicul in tabela hash, atunci
il adaugam in tabela hash si ca singur client punem clientul curent. La randul sau, clientul
TCP are o lista de topicuri la care este abonat, acestia fiind pointeri catre zone din
tabela hash.
	Atunci cand primim un mesaj de la un client UDP, ne uitam la topicul mesajului si il
cautam in tabela hash. Daca nu este un entry in tabela, inseamna ca nu exista si mesajul
nu va fi dat mai departe nimanui. Daca gasim topicul, atunci ne uitam in lista sa de
subscriberi si trimitem mesaje catre toti clientii sai.
	Daca un client TCP vrea sa faca unsubscribe, atunci se cauta topicul dorit in lista
de topicuri la care este abonat. Daca nu exista topicul de la care se doreste dezabonarea,
atunci afisam un mesaj de eroare si asteptam alte comenzi. Daca gasim topicul, atunci
il scoatem din lista de topicuri a clientului, iar apoi ne ducem in tabela hash, ne uitam
prin lista de subscriberi si eliminam clientul curent. Daca clientul TCP era singurul subscriber,
atunci pentru a scapa de memorie inutila, eliminam si topicul din tabela, inclusiv cu toata
memoria alocata acestuia.
	Daca un client TCP se deconecteaza de la server, atunci ii atribuim socketul 0. Stim
ca acest descriptor este rezervat pentru STDIN, de aceea niciun alt client TCP nu poate
avea "din greseala" acest socket. Astfel ne putem da seama care clienti TCP sunt inca
conectati si care sunt momentan deconectati. Astfel, cand vine un mesaj UDP si ne
uitam la subscriberi, daca gasim un client cu socketul 0, ne uitam apoi la valoarea
SF a acestuia. Daca este 0, atunci ignoram pur si simplu subscriberul. Daca SF este 1,
atunci alocam spatiu pentru un mesaj si tinem o lista de mesaje care trebuie sa fie
trimise la politica store-and-forward. 
	
	Modul de functionare a mesajelor:
	Pentru a transmite mesajele am folosit o structura de date pentru a incadra toate
mesajele pe care le transmite/primeste serverul. Acest mesaj contine ca prim atribut
lungimea totala a mesajului, apoi tipul mesajului (pe care il vom detalia mai jos), adresa
ip si portul clientului UDP care a transmis mesajul. Apoi, avem efectiv mesajul de la UDP
care este impartit in topic, tip de date si payload.
	Acum vom incepe sa explic tipurile diferite de mesaje care pot aparea in aplicatie.
	Cand un client TCP se conecteaza la un socket folosind socketul de listen nu ii stim
ID-ul. Asa ca, primul lucru pe care il face clientul dupa ce conexiunea a fost acceptata
este sa trimita un mesaj cu numele sau. Acest tip de mesaj are "type" 0. Numele
este trimis in payload. Server-ul cand primeste un astfel de mesaj, se plimba prin
lista de clienti si verifica daca ID-ul exista deja. Daca da, iar socketul exista, atunci
afisam un mesaj de eroare si ii trimitem un mesaj clientului sa se inchida. Daca
socketul este totusi 0, inseamna ca avem de-a face cu un client care s-a intors. In acest
caz, ne plimbam prin toate topicurile la care este abonat si verificam daca avem
sau nu mesaje netrimise pe care trebuie sa le trimitem. Daca da, atunci le trimitem si
le stergem din memorie apoi. Altfel, nu facem nimic, doar tinem cont ca clientul
s-a intors si are un nou socket pe care comunica cu serverul.
	Un alt tip de mesaj este cel cu "type" 1 si este un mesaj de inchidere. Cand clientul
primeste comanda exit, acesta, local, isi dezaloca memoria si se pregateste de inchidere,
iar la final trimite un mesaj de inchidere care are doar len si type. Daca serverul este cel
care vrea sa se inchida, atunci trimite un mesaj de inchidere tuturor clientilior din
lista, iar apoi dezaloca toata memoria aplicatiei si se inchide.
	Daca un client vrea sa faca subscribe la un topic va trimite un mesaj de tipul 2.
Acest mesaj va avea len, type, ip si port (care sunt 0 pentru ca nu ne intereseaza), si
topic unde este locul in care este efectiv numele topicului la care vrem sa facem subscribe.
Apoi, pentru a nu adauga un camp nou, data_type este 0 daca SF este 0, sau 1 daca SF este
1. Daca subscribe-ul a fost reusit, atunci trimitem inapoi la client un mesaj de confirmare
tot cu tipul 2. Cand clientul primeste acest tip de mesaj afiseaza mesajul de confirmare.
Daca nu este primita confirmarea, atunci serverul afiseaza o eroare, iar clientul nu
va anunta ca s-a facut cu succes subscribe-ul. La fel facem si pentru comanda de
unsubscribe, cu modificarea ca "type" este 3.
	Un ultim tip de mesaj are "type" 4 si reprezinta efectiv mesajul de la UDP.
Serverul primeste de la clientii UDP datagrame si le introduce in mesaj. Aici
sunt completate si IP-ul si portul clientului UDP pentru ca clientul TCP sa stie
cine a publicat informatiile pe un anumit topic.
	Si serverul si clientii TCP, trebuie sa fie atenti daca TCP-ul uneste sau imparte
mesajele in mai multe receiv-uri. Acestia asteapta pana cand in buffer gasim
cel putin 2 bytes care reprezinta lungimea mesajului pe care urmeaza sa il
primim. Dupa aceea, asteptam sa primim atatia octeti cat este lungimea. Daca
in buffer au ajuns mai multi bytes ca lungime, atunci inseamna ca avem mai
multe mesaje lipite. Tinem cont de acest lucru si le interpretam simplu, ca
si cand primul mesaj nici nu exista. Daca bufferul s-a umplut, dar mai avem de primit
din mesajul curent, inseamna ca s-au primit mai multe mesaje lipte, iar ultimul nu s-a
primit tot. Astfel, se sterg din buffer primele mesaje care au fost deja interpretate
si se shifteaza mesajul nostru la inceputul bufferului si asteptam continuarea
acestuia.

	Coduri de eroare:
	Programul se termina cu codul de eroare 1 daca nu a fost apelat corect (au fost
prea putine argumente pentru argv).
	Programul se termina cu codul de eroare 2 daca au aparut probleme cu API-ul de
socketi.
	Programul se termina cu codul de eroare 3 daca au aparut probleme la alocarea
memoriei.