#define _POSIX_C_SOURCE 200112L
#define _POSIX_TIMES
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include "./include/Queue.h"

//macro che controlla se s restituisce -1
#define ec_meno1(s,m) \
  if ((s)==-1) {perror(m); exit(EXIT_FAILURE);} 

//macro che controlla se s != 1, in quel caso stampa il messaggio di errore
#define controllo_file(s, m) \
  if ((s)!=1) {perror(m); exit(EXIT_FAILURE);} 

//macro che controlla se s==NULL  
#define ec_null(s, m) \
  if ((s)==NULL) fprintf(stderr, m "\n");

//macro che controlla se una lock non è andata a buon fine
#define lock_mutex_control(s) \
  if (s!=0) {fprintf(stderr, "Errore durante la lock");} 

//macro che controlla se una unlock non è andata a buon fine  
#define unlock_mutex_control(s) \
  if (s!=0) {fprintf(stderr, "Errore durante la unlock");} 


typedef struct cliente{
  int id;
  int prodotti_acquistati;
  int t_acquisti;
  float t_tot_supermercato;
  float t_coda;
  int n_code_visitate;
  float t_gestione;
  int id_cassa_usata;
  pthread_mutex_t in_queue;  //mutex da acquisire per accedere ad un determinato cliente
  pthread_cond_t fine_queue; //variabile di condizione che indica quando il client esce da una coda
} cliente; 


typedef struct supermercato {
  int clienti_entrati;  //clienti totali
  int clienti_nel_supermercato;  //clienti attuali nel supermercato
  int clienti_serviti;  //clienti che hannoa acquistato almeno un prodotto e sono usciti
  int n_prodotti_acquistati;
}supermercato;


typedef struct cassa {
  int id;
  int n_clienti_Serviti;
  int n_prodotti_elaborati;
  float t_apertura;
  int t_fisso_cassiere;
  float t_medio_servizio;
  int n_chiusure;
  int cassa_aperta;
} cassa;


typedef struct configFile {
  int K;  //numero di casse totali
  int C;  //numero di clienti totali nel supermercato
  int E;  //se numero di clienti nel supermercato < C-E allora posso far entrare altri clienti
  int T;  //tempo massimo per gli acquisti
  int P;  //numero massimo di proodtti acquistabili
  int S;  //intervallo di tempo 
  int S1; //esempio, se s1=2 il direttore chiude una cassa se ci sono due cassi con al più 1 cliente (definisce il numero di casse aperte con al più un cliente)
  int S2; //esempio, se s2=10, apro una cassa (se possibile) se c'è almeno una cassa con 10 clienti in coda (definisce la soglia di apertura di una cassa)
  int casse_iniziali;  //numero di casse con le quali apre il supermercato
  int clienti_iniziali;  //numero di clienti con i quali apre il supermercato
  int t_fisso_prodotto;  //tempo fisso del cassiere per ogni prodotto
  int t_info_direttore;  //ogni t_info_direttore le casse informano il direttore su quente persone sono in coda
} file; 



void leggiFile(file *config, char* arg);  //funzione di supporto per leggere i valori dal file di configurazione
file *config;  //contiene le informazioni del file di configurazione

pthread_mutex_t logFile_mutex = PTHREAD_MUTEX_INITIALIZER;
FILE *logFile;  //file di log con tutte le informazioni delle casse e dei clienti

void clienteInit(struct cliente *cl, int id); //funzione di supporto per inizializzare i clienti
void cassaInit(struct cassa *ca, int id); //funzione di supporto per inizializzare le casse
//void logPrintCliente(int id);
void spostaClienti(struct Queue* q, int id_cassa);

pthread_mutex_t memoria_libera_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t memoria_libera_cond =PTHREAD_COND_INITIALIZER;

void *Direttore();
void *SubDirectorGestioneClienti(void *arg);
void *SubDirectorGestioneCasse();
void *GestioneCliente(void *arg);
void *GestioneCassa(void *arg);
void* clock_casse(void *arg);
void* clock_casse_info();


int *tid_casse; //array con i tid delle casse


volatile sig_atomic_t sighup = 0;
volatile sig_atomic_t sigquit = 0;


//variabili che verranno settate dal thread direttore
volatile sig_atomic_t exithup = 0;
volatile sig_atomic_t exitquit = 0;


//funzione per la gestione dei segnali sighup e sigquit
static void gestore (int signum){
  if (signum==1)  { sighup = 1; }
  if (signum==3)  { sigquit = 1; }  /*ricezione di ctrl + \ */
} 


//struct e mutex associata per la gestione delle informazioni generali del supermercato
pthread_mutex_t controllo_info_supermercato_mutex = PTHREAD_MUTEX_INITIALIZER;
supermercato *info_supermercato; 


//array di mutex e di variabili di condizione per il controllo del tempo di apertura delle casse
pthread_mutex_t *controllo_apertura_mutex;
pthread_cond_t *cassa_chiusa_cond;


//array di struct cassa, usato per gestire le informazioni delle casse
pthread_mutex_t *gestione_casse_mutex;
cassa **casse_supermarket;


//array di struct cliente, mutex e variabile di condizione per ogni cliente
pthread_mutex_t info_clienti_mutex = PTHREAD_MUTEX_INITIALIZER;
cliente **info_clienti;


//coda per i clienti senza acquisti
pthread_mutex_t queue_no_acquisti_mutex;
Queue *queue_no_acquisti;


//sistema di code per le casse
pthread_mutex_t* queue_casse_mutex;
Queue** queue_casse;


//mutex e variabile di condizione per la gestione delle casse da parte del direttore
pthread_mutex_t update_direttore = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t update_direttore_fine = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t richiesta_controllo_ricevuta = PTHREAD_COND_INITIALIZER;
pthread_cond_t controllo_effettuato = PTHREAD_COND_INITIALIZER;


//mutex associata alla variabile binary_Casse
pthread_mutex_t casse_aperte_mutex = PTHREAD_MUTEX_INITIALIZER;
int* binary_casse; //array binario che indica se una cassa è aperta o chiusa
int casse_aperte = 0; //numero di casse aperte


void *SubDirectorGestioneCasse(){
  int casse_1_cliente;
  unsigned int seed1 = time(NULL), seed2, seed3;
  int index_cassa = 0;
  //int cassa_trovata;

  while (1){
    if ((exithup==1 || exitquit==1) && info_supermercato->clienti_nel_supermercato==0){
    return NULL;
    }
    lock_mutex_control(pthread_mutex_lock(&update_direttore));
    
    pthread_cond_wait(&richiesta_controllo_ricevuta, &update_direttore);
    unlock_mutex_control(pthread_mutex_unlock(&update_direttore));
    //ricevuto segnale di controllo, passo a controllare la situazione delle casse
    casse_1_cliente = 0;
    for(size_t i=0; i<config->K; i++){
      //printf("queue_casse_mutex lockata\n");
      lock_mutex_control(pthread_mutex_lock(&queue_casse_mutex[i]));
      if (queueSize(queue_casse[i])==1){
        casse_1_cliente++;
        //se ci sono S1 casse con 1 cliente ne chiudo una
        if (casse_1_cliente>=config->S1){
          //printf("CASSA %d CHIUSA\n", (int)i);
          lock_mutex_control(pthread_mutex_lock(&gestione_casse_mutex[i]));
          lock_mutex_control(pthread_mutex_lock(&casse_aperte_mutex));
          casse_aperte--;
          binary_casse[i] = 0;
          casse_supermarket[i]->cassa_aperta = 0;
          casse_supermarket[i]->n_chiusure++;
          unlock_mutex_control(pthread_mutex_unlock(&casse_aperte_mutex));
          unlock_mutex_control(pthread_mutex_unlock(&gestione_casse_mutex[i]));
          lock_mutex_control(pthread_mutex_lock(&controllo_apertura_mutex[i]));
          pthread_cond_signal(&cassa_chiusa_cond[i]);
          unlock_mutex_control(pthread_mutex_unlock(&controllo_apertura_mutex[i]));
        }
      }
      else if (queueSize(queue_casse[i])>=config->S2){
        lock_mutex_control(pthread_mutex_lock(&casse_aperte_mutex));
        if (casse_aperte < config->K){
          //printf("NUMERO DI CASSE APERTE: %d\n", casse_aperte);
          unlock_mutex_control(pthread_mutex_unlock(&casse_aperte_mutex));
          seed2 = time(NULL)+i;
          seed3 = time(NULL)+seed1+seed2;
          //cassa_trovata = 0;
          //cerco una cassa chiusa e la apro
          do
          {
            //printf("CERCO UNA CASSA CHIUSA\n");
            index_cassa = rand_r(&seed3) % (config->K);
            //printf("ID CASSA DA APRIRE: %d\n", index_cassa);
            
            lock_mutex_control(pthread_mutex_lock(&casse_aperte_mutex));
            //printf("VALORE BINARY_CASSE[ID]: %d\n", binary_casse[index_cassa]);
            if (binary_casse[index_cassa]==0){
              //printf("CASSA APERTA: %d\n", index_cassa);
              lock_mutex_control(pthread_mutex_lock(&gestione_casse_mutex[index_cassa]));
              //printf("NUMERO DI CASSE APERTE: %d\n", casse_aperte);
              casse_aperte++;
              binary_casse[index_cassa]=1;
              casse_supermarket[index_cassa]->cassa_aperta=1;
              //cassa_trovata = 1;
              unlock_mutex_control(pthread_mutex_unlock(&gestione_casse_mutex[index_cassa]));
              //printf("CASSA APERTA2: %d\n", index_cassa);
            }
            unlock_mutex_control(pthread_mutex_unlock(&casse_aperte_mutex));
          } while (binary_casse[index_cassa]==0); 
        }
        else{
          unlock_mutex_control(pthread_mutex_unlock(&casse_aperte_mutex));
        }
      }
      //printf("queue_casse_mutex unlockata\n");
      unlock_mutex_control(pthread_mutex_unlock(&queue_casse_mutex[i]));
    }
    
    lock_mutex_control(pthread_mutex_lock(&update_direttore_fine));
    pthread_cond_signal(&controllo_effettuato);
    unlock_mutex_control(pthread_mutex_unlock(&update_direttore_fine));
  }
  return NULL;
}


//sotto-thread del direttore, si occuperà dell'entrata/uscita dei vari clienti
void *SubDirectorGestioneClienti(void *arg){
  int err;
  pthread_t tid_cliente;
  int id_cliente;
  int dim_realloc;
  int id_cliente_no_acquisti;

  while (sighup == 0 && sigquit == 0){
    if (info_supermercato->clienti_nel_supermercato < (config->C - config->E)){
      lock_mutex_control(pthread_mutex_lock(&controllo_info_supermercato_mutex));
      id_cliente = info_supermercato->clienti_entrati;
      dim_realloc = info_supermercato->clienti_entrati+=config->E;
      unlock_mutex_control(pthread_mutex_unlock(&controllo_info_supermercato_mutex));
      fflush(stdout);

      cliente** tmp;
      
      lock_mutex_control(pthread_mutex_lock(&info_clienti_mutex));
      ec_null(tmp = realloc(info_clienti, dim_realloc*sizeof(cliente)), "realloc su info_clienti non riuscita");
      info_clienti = tmp;
      unlock_mutex_control(pthread_mutex_unlock(&info_clienti_mutex));


      //inizializzo E thread clienti
      for(size_t i = 0; i<config->E; i++){
        if ((err = pthread_create(&tid_cliente, NULL, GestioneCliente, (void*) (__intptr_t) id_cliente))!=0){
          fprintf(stderr, "Errore durante l'inizializzazione del thread %d", id_cliente);
          exit(EXIT_FAILURE);
        }
        
        if ((err = pthread_detach(tid_cliente))!=0){
          fprintf(stderr, "errore durante la pthread_detach del cliente %d", id_cliente);
          exit(EXIT_FAILURE);
        }
        id_cliente++;
      }
      //aggiorno i clienti totali entrati nel supermercato e i clienti attuali nel supermercato
        lock_mutex_control(pthread_mutex_lock(&controllo_info_supermercato_mutex));
        info_supermercato->clienti_nel_supermercato += config->E;
        unlock_mutex_control(pthread_mutex_unlock(&controllo_info_supermercato_mutex));
    }

    while (!(isEmpty(queue_no_acquisti))){
      lock_mutex_control(pthread_mutex_lock(&queue_no_acquisti_mutex));
      id_cliente_no_acquisti = dequeue(queue_no_acquisti);
      unlock_mutex_control(pthread_mutex_unlock(&queue_no_acquisti_mutex));

      lock_mutex_control(pthread_mutex_lock(&info_clienti_mutex));
      lock_mutex_control(pthread_mutex_lock(&info_clienti[id_cliente_no_acquisti]->in_queue));
      pthread_cond_signal(&info_clienti[id_cliente_no_acquisti]->fine_queue);
      unlock_mutex_control(pthread_mutex_unlock(&info_clienti[id_cliente_no_acquisti]->in_queue));
      unlock_mutex_control(pthread_mutex_unlock(&info_clienti_mutex));
    }
  }

  //ricevuta sigquit o sighup finisco di gestire gli ultimi clienti senza acquisti
  lock_mutex_control(pthread_mutex_lock(&controllo_info_supermercato_mutex));
  while (info_supermercato->clienti_nel_supermercato!=0){
    unlock_mutex_control(pthread_mutex_unlock(&controllo_info_supermercato_mutex));
    lock_mutex_control(pthread_mutex_lock(&queue_no_acquisti_mutex));
    if (!(isEmpty(queue_no_acquisti))){
      id_cliente_no_acquisti = dequeue(queue_no_acquisti);
      unlock_mutex_control(pthread_mutex_unlock(&queue_no_acquisti_mutex));

      lock_mutex_control(pthread_mutex_lock(&info_clienti_mutex));
      lock_mutex_control(pthread_mutex_lock(&info_clienti[id_cliente_no_acquisti]->in_queue));
      pthread_cond_signal(&info_clienti[id_cliente_no_acquisti]->fine_queue);
      unlock_mutex_control(pthread_mutex_unlock(&info_clienti[id_cliente_no_acquisti]->in_queue));
      unlock_mutex_control(pthread_mutex_unlock(&info_clienti_mutex));
    }
    else{
      unlock_mutex_control(pthread_mutex_unlock(&queue_no_acquisti_mutex));
    }
    lock_mutex_control(pthread_mutex_lock(&controllo_info_supermercato_mutex));
  }
  unlock_mutex_control(pthread_mutex_unlock(&controllo_info_supermercato_mutex));
  //printf("SUBDIRECTORGESTIONECLIENTI TERMINATO\n");
  return NULL;
}


//thread direttore, farà entrare i primi clienti, aprirà le prime casse e gestirà il supermercato
void *Direttore(){
  int err;
  int index_cassa = 0;
  pthread_t *tid_casse;
  pthread_t *tid_clienti;
  pthread_t tid_sub_director;
  pthread_t *tid_clock_casse;
  pthread_t tid_subDirectorGestioneCasse;
  pthread_t tid_clock_casse_info;

  tid_casse = malloc(config->K*sizeof(pthread_t));
  ec_null(tid_casse, "malloc su tid_casse non riuscita");

  tid_clienti = malloc(config->clienti_iniziali*sizeof(pthread_t));
  ec_null(tid_clienti, "malloc su tid_clienti non riuscita");

  tid_clock_casse = malloc(config->K*sizeof(pthread_t));
  ec_null(tid_clock_casse, "malloc su tid_clock_casse non riuscita");


  //inizializzo le informazioni del supermercato
  lock_mutex_control(pthread_mutex_lock(&controllo_info_supermercato_mutex));
  info_supermercato = malloc(sizeof(supermercato));
  ec_null(info_supermercato, "malloc su info_supermercato non riuscita");
  info_supermercato->clienti_entrati = config->clienti_iniziali;
  info_supermercato->clienti_nel_supermercato = config->clienti_iniziali;
  info_supermercato->clienti_serviti = 0;
  info_supermercato->n_prodotti_acquistati = 0;
  unlock_mutex_control(pthread_mutex_unlock(&controllo_info_supermercato_mutex));


  //inizializzo i thread cassa, assegnando loro un id, inizializzo anche le mutex per ogni cassa
  casse_supermarket = malloc(config->K*sizeof(cassa*));
  ec_null(casse_supermarket, "malloc su casse_supermarket non riuscita");

  gestione_casse_mutex = malloc(config->K*sizeof(pthread_mutex_t));
  ec_null(gestione_casse_mutex, "malloc su gestione_casse_mutex non riuscita");

  binary_casse = calloc(config->K, config->K*sizeof(int));
  ec_null(binary_casse, "malloc su binary_casse non riuscita");

  queue_casse = malloc(config->K*sizeof(Queue*));
  ec_null(queue_casse, "malloc su queue_casse non riuscita");

  queue_casse_mutex = malloc(config->K*sizeof(pthread_mutex_t));
  ec_null(queue_casse_mutex, "malloc su queue_casse_mutex non riuscita");

  controllo_apertura_mutex = malloc(config->K*sizeof(pthread_mutex_t));
  ec_null(controllo_apertura_mutex, "malloc su controllo_apertura_mutex non riuscita");

  cassa_chiusa_cond = malloc(config->K*sizeof(pthread_cond_t));
  ec_null(cassa_chiusa_cond, "malloc su cassa_chiusa_cond non riuscita");


  queue_no_acquisti = createQueue(1);


  //creo i trhead casse
  while (index_cassa < config->K){

    if ((pthread_mutex_init(&controllo_apertura_mutex[index_cassa], NULL)) != 0){
      fprintf(stderr, "Errore durante l'inizializzazione di controllo_apertura_mutex");
      exit(EXIT_FAILURE);
    }

    if ((pthread_mutex_init(&gestione_casse_mutex[index_cassa], NULL)) !=0 ){
      fprintf(stderr, "Errore durante l'inizializzazione di gestione_casse_mutex");
      exit(EXIT_FAILURE);
    }

    if ((pthread_cond_init(&cassa_chiusa_cond[index_cassa], NULL))!=0){
      fprintf(stderr, "Errore durante l'inizializzazione di cassa_chiusa_cond");
      exit(EXIT_FAILURE);
    }

    lock_mutex_control(pthread_mutex_lock(&gestione_casse_mutex[index_cassa]));
    casse_supermarket[index_cassa] = malloc(sizeof(cassa));
    cassaInit(casse_supermarket[index_cassa], index_cassa);
    unlock_mutex_control(pthread_mutex_unlock(&gestione_casse_mutex[index_cassa]));


    //creo un nuovo thread cassa e gli passo la struct appartenente a quella cassa
    if ((err = pthread_create(&tid_casse[index_cassa], NULL, GestioneCassa, (void*) (intptr_t) index_cassa))!=0){
      fprintf(stderr, "Errore durante la creazione della cassa %d", index_cassa);
      exit(EXIT_FAILURE);
    }

    /*
    if ((err = pthread_detach(tid_casse[index_cassa]))!=0){
      fprintf(stderr, "Errore durante la detach");
      exit(EXIT_FAILURE);
    }*/

    /*
    if (pthread_create(&tid_clock_casse[index_cassa], NULL, clock_casse, (void*) (__intptr_t) index_cassa)){
      fprintf(stderr, "Errore durante la creazione del clock_Cassa %d", index_cassa);
      exit(EXIT_FAILURE);
    }

    if ((err = pthread_detach(tid_clock_casse[index_cassa]))!=0){
      fprintf(stderr, "Errore durante la detach di clock_casse");
      exit(EXIT_FAILURE);
    }*/
    index_cassa++;

  }


  //apro le prime casse
  for (size_t i = 0; i < config->casse_iniziali; i++){
    
    lock_mutex_control(pthread_mutex_lock(&gestione_casse_mutex[i]));
    casse_supermarket[i]->cassa_aperta = 1;
    unlock_mutex_control(pthread_mutex_unlock(&gestione_casse_mutex[i]));

    lock_mutex_control(pthread_mutex_lock(&casse_aperte_mutex));
    binary_casse[i]=1;
    casse_aperte++;
    unlock_mutex_control(pthread_mutex_unlock(&casse_aperte_mutex));
  }
  

  lock_mutex_control(pthread_mutex_lock(&info_clienti_mutex));
  info_clienti = malloc(config->clienti_iniziali*sizeof(cliente*));
  ec_null(info_clienti, "malloc su info_clienti non riuscita");
  unlock_mutex_control(pthread_mutex_unlock(&info_clienti_mutex));
  
  


  //Inizializzo il sotto-thread clock_casse_info
  if ((err = pthread_create(&tid_clock_casse_info, NULL, clock_casse_info, NULL))!=0){
    fprintf(stderr, "Errore durante la creazione del thread clock_casse_info");
    exit(EXIT_FAILURE);
  }

  if ((err = pthread_detach(tid_clock_casse_info))!=0){
    fprintf(stderr, "Errore durante la creazione del thread clock_casse_info");
    exit(EXIT_FAILURE);
  }



  //Inizializzo il sotto-thread SubDirectorGestioneCasse che gestirà l'apertura e la chiusura delle casse
  if ((err = pthread_create(&tid_subDirectorGestioneCasse, NULL, SubDirectorGestioneCasse, NULL))!=0){
    fprintf(stderr, "Errore durante la creazione del thread SubDirectorGestioneCasse");
    exit(EXIT_FAILURE);
  }

  if ((err = pthread_detach(tid_subDirectorGestioneCasse))!=0){
    fprintf(stderr, "Errore durante la creazione del thread SubDirectorGestioneCasse");
    exit(EXIT_FAILURE);
  }


  //faccio entrare i primi clienti
  for (size_t i = 0; i < config->clienti_iniziali; i++){

    if ((err = pthread_create(&tid_clienti[i], NULL, GestioneCliente, (void*) (__intptr_t) i))!=0){
      fprintf(stderr, "Errore durante la creazione del thread cliente");
      exit(EXIT_FAILURE);
    }

    if ((err = pthread_detach(tid_clienti[i])!=0)){
      fprintf(stderr, "Errore durante la detach del thread cliente: %ld", tid_clienti[i]);
      exit(EXIT_FAILURE);
    }
  }


  //TODO: inizializzare il SubDirectorGestioneClienti che si occuperà di far entrare/uscire i clienti dal supermercato
  if ((err = pthread_create(&tid_sub_director, NULL, SubDirectorGestioneClienti, NULL))!=0){
    fprintf(stderr, "Errore durante l'inizializzazione del thread SubDirector");
    exit(EXIT_FAILURE);
  }


  if ((err = pthread_detach(tid_sub_director))!=0){
    fprintf(stderr, "Errore durante la detach nel thread SubDirector");
    exit(EXIT_FAILURE);
  }
   

  while (1){
    if (sighup == 1){
      exithup = 1;
      printf("RICEVUTO SEGNALE SIGHUP\n");
      fflush(stdout);
      break;
    } 
    else if (sigquit == 1){
      exitquit = 1;
      printf("RICEVUTO SEGNALE SIGQUIT\n");
      fflush(stdout);
      break;
    }   
  }
    
  for (size_t i = 0; i<config->K; i++){
    if ((err = pthread_join(tid_casse[i],NULL))!=0){
    fprintf(stderr, "Errore durante la join");
    exit(EXIT_FAILURE); 
    }
  }
  
  printf("dopo la join casse\n");
  while (1){
    lock_mutex_control(pthread_mutex_lock(&controllo_info_supermercato_mutex));
    //printf("CLIENTI NEL SUPERMERCATO (DIRETTORE): %d\n", info_supermercato->clienti_nel_supermercato);
    if (info_supermercato->clienti_nel_supermercato==0){
      //printf("CLIENTI NEL SUPERMERCATO (DIRETTORE): %d\n", info_supermercato->clienti_nel_supermercato);
      unlock_mutex_control(pthread_mutex_unlock(&controllo_info_supermercato_mutex));
      break;
    }
    else {
      unlock_mutex_control(pthread_mutex_unlock(&controllo_info_supermercato_mutex));
    }
  }  
  free(tid_casse);  free(tid_clienti); free(tid_clock_casse);
  printf("DIRETTORE TERMINATO\n");
  fflush(stdout);
  return NULL;
}


//funzione da passare ai thread inizializzati dalle casse per tenere traccia del tempo di apertura
void* clock_casse(void *arg){
  struct timespec time_open, time_close;
  int id_cassa = (int) (__intptr_t) arg;
  int flag = 0;
  //printf("id cassa nel thread clock casse: %d\n", id_cassa);
  fflush(stdout);

  while (exithup == 0 && exitquit == 0){
    flag = 0;
    lock_mutex_control(pthread_mutex_lock(&gestione_casse_mutex[id_cassa]));
    if (casse_supermarket[id_cassa]->cassa_aperta==1){
      flag = 1;
      unlock_mutex_control(pthread_mutex_unlock(&gestione_casse_mutex[id_cassa]));
      clock_gettime(CLOCK_REALTIME, &time_open);
      // printf("CASSA %d APERTA\n", id_cassa);
      lock_mutex_control(pthread_mutex_lock(&controllo_apertura_mutex[id_cassa]));
      pthread_cond_wait(&cassa_chiusa_cond[id_cassa], &controllo_apertura_mutex[id_cassa]);
      clock_gettime(CLOCK_REALTIME, &time_close);
      //printf("CASSA %d chiusa\n", id_cassa);
      unlock_mutex_control(pthread_mutex_unlock(&controllo_apertura_mutex[id_cassa]));
      lock_mutex_control(pthread_mutex_lock(&gestione_casse_mutex[id_cassa]));
      casse_supermarket[id_cassa]->t_apertura += ((time_close.tv_sec - time_open.tv_sec)*1000) + ((time_close.tv_nsec - time_open.tv_nsec)/1000000);
      //printf("TEMPO DI APERTURA DELLA CASSAAAAAAA %d: %0.3f\n", id_cassa, casse_supermarket[id_cassa]->t_apertura);
      unlock_mutex_control(pthread_mutex_unlock(&gestione_casse_mutex[id_cassa]));
    }
    else if(flag == 0){
      unlock_mutex_control(pthread_mutex_unlock(&gestione_casse_mutex[id_cassa]));
    }
  }
  printf("THREAD CLOCK CASSE TERMINATO\n");
  return NULL;
}


//funzione da passare al thread di supporto che farà da timer per il passaggio delle informazioni al direttore
void *clock_casse_info(){
  struct timespec res;
  
  res.tv_sec = config->t_info_direttore/1000;
  res.tv_nsec = (config->t_info_direttore%1000)*1000000;

  while (1){
    if (exithup==1 || exitquit==1){
      //printf("THREAD CLOCK_CASSE_INFO TERMINATO\n");
      return NULL;
    }
    nanosleep(&res, NULL);
    lock_mutex_control(pthread_mutex_lock(&update_direttore));
    pthread_cond_signal(&richiesta_controllo_ricevuta);
    unlock_mutex_control(pthread_mutex_unlock(&update_direttore));

    lock_mutex_control(pthread_mutex_lock(&update_direttore_fine));
    pthread_cond_wait(&controllo_effettuato, &update_direttore_fine);
    unlock_mutex_control(pthread_mutex_unlock(&update_direttore_fine));
  }
}



void *GestioneCassa(void *arg){
  int id_cassa = (int) (intptr_t) arg;
  int id_cliente;
  int err;
  pthread_t tid_clock_casse;
  int cliente_servito;
  struct timespec time_gestione, t_fisso;

  if ((err = pthread_create(&tid_clock_casse, NULL, clock_casse, (void*) (__intptr_t) id_cassa))!=0){
    fprintf(stderr, "Errore durante la creazione del clock_Cassa %d", id_cassa);
    exit(EXIT_FAILURE);
  }


  lock_mutex_control(pthread_mutex_lock(&gestione_casse_mutex[id_cassa]));
  t_fisso.tv_sec = casse_supermarket[id_cassa]->t_fisso_cassiere/1000;
  t_fisso.tv_nsec = (casse_supermarket[id_cassa]->t_fisso_cassiere%1000)*1000000;
  unlock_mutex_control(pthread_mutex_unlock(&gestione_casse_mutex[id_cassa]));


  //inizializzo la mutex associata alla coda della cassa id_cassa
  pthread_mutex_init(&queue_casse_mutex[id_cassa], NULL);


  //inizializzo la coda di clienti associata alla cassa id_cassa
  lock_mutex_control(pthread_mutex_lock(&queue_casse_mutex[id_cassa]));
  queue_casse[id_cassa] = createQueue(1000);
  unlock_mutex_control(pthread_mutex_unlock(&queue_casse_mutex[id_cassa]));


  while (exitquit == 0){
    cliente_servito = 0;
    if (!isEmpty(queue_casse[id_cassa])){
      id_cliente = dequeue(queue_casse[id_cassa]);

      //gestisco il cliente corrispondende all'id_cliente
      lock_mutex_control(pthread_mutex_lock(&info_clienti_mutex));
      time_gestione.tv_sec = (info_clienti[id_cliente]->prodotti_acquistati*config->t_fisso_prodotto)/1000 + (t_fisso.tv_sec);
      time_gestione.tv_nsec = ((info_clienti[id_cliente]->prodotti_acquistati*config->t_fisso_prodotto)%1000)*1000000 + (t_fisso.tv_nsec);
      unlock_mutex_control(pthread_mutex_unlock(&info_clienti_mutex));
      nanosleep(&time_gestione, NULL);

      //salvo il tempo che mi serve per gestire il cliente
      lock_mutex_control(pthread_mutex_lock(&info_clienti_mutex));
      info_clienti[id_cliente]->t_gestione = time_gestione.tv_sec*1000 + (time_gestione.tv_nsec/1000000); //inserisco nelle info del cliente il tempo di gestione di quel cliente
      info_clienti[id_cliente]->id_cassa_usata = id_cassa;
      unlock_mutex_control(pthread_mutex_unlock(&info_clienti_mutex));

      //risveglio il cliente in attesa
      lock_mutex_control(pthread_mutex_lock(&info_clienti_mutex));
      lock_mutex_control(pthread_mutex_lock(&info_clienti[id_cliente]->in_queue));
      pthread_cond_signal(&info_clienti[id_cliente]->fine_queue);
      unlock_mutex_control(pthread_mutex_unlock(&info_clienti[id_cliente]->in_queue));
      unlock_mutex_control(pthread_mutex_unlock(&info_clienti_mutex));
      cliente_servito=1;
    }

    //se ho servito il cliente aggiorno le informazioni del supermercato
    if (cliente_servito==1){
      lock_mutex_control(pthread_mutex_lock(&gestione_casse_mutex[id_cassa]));
      casse_supermarket[id_cassa]->n_clienti_Serviti++;
      lock_mutex_control(pthread_mutex_lock(&info_clienti_mutex));
      casse_supermarket[id_cassa]->n_prodotti_elaborati += info_clienti[id_cliente]->prodotti_acquistati;
      unlock_mutex_control(pthread_mutex_unlock(&info_clienti_mutex));
      unlock_mutex_control(pthread_mutex_unlock(&gestione_casse_mutex[id_cassa]));
    }

    //se la cassa è chiusa sposto i clienti in attesa nella coda
    lock_mutex_control(pthread_mutex_lock(&gestione_casse_mutex[id_cassa]));
    if (casse_supermarket[id_cassa]->cassa_aperta==0 && exitquit == 0){
      unlock_mutex_control(pthread_mutex_unlock(&gestione_casse_mutex[id_cassa]));
      lock_mutex_control(pthread_mutex_lock(&queue_casse_mutex[id_cassa]));
      if (!(isEmpty(queue_casse[id_cassa]))){
        unlock_mutex_control(pthread_mutex_unlock(&queue_casse_mutex[id_cassa]));
        spostaClienti(queue_casse[id_cassa], id_cassa);
      }
      else{
        unlock_mutex_control(pthread_mutex_unlock(&queue_casse_mutex[id_cassa]));
      }
    }
    else{
      unlock_mutex_control(pthread_mutex_unlock(&gestione_casse_mutex[id_cassa]));
    }

    //in caso di sighup se ho servito tutti i clienti termino il thread altrimenti continuo a servirli
    lock_mutex_control(pthread_mutex_lock(&controllo_info_supermercato_mutex));
    //printf("CLIENTI NEL SUPERMERCATO: %d\n",info_supermercato->clienti_nel_supermercato);
    fflush(stdout);
    if (exithup == 1 && isEmpty(queue_casse[id_cassa]) && info_supermercato->clienti_nel_supermercato==0){
      //printf("CLIENTI NEL SUPERMERCATO2: %d\n",info_supermercato->clienti_nel_supermercato);
      //fflush(stdout);
      unlock_mutex_control(pthread_mutex_unlock(&controllo_info_supermercato_mutex));
      //se la cassa è aperta la chiudo
      lock_mutex_control(pthread_mutex_lock(&casse_aperte_mutex));
      if (binary_casse[id_cassa]==1){
        lock_mutex_control(pthread_mutex_lock(&controllo_apertura_mutex[id_cassa]));
        lock_mutex_control(pthread_mutex_lock(&gestione_casse_mutex[id_cassa]));
        casse_supermarket[id_cassa]->cassa_aperta=0;
        unlock_mutex_control(pthread_mutex_unlock(&gestione_casse_mutex[id_cassa]));
        pthread_cond_signal(&cassa_chiusa_cond[id_cassa]); //avviso il thread di supporto clock_casse che la cassa è chiusa
        unlock_mutex_control(pthread_mutex_unlock(&controllo_apertura_mutex[id_cassa]));

        binary_casse[id_cassa]=0;
        casse_aperte--;
      
      }
      unlock_mutex_control(pthread_mutex_unlock(&casse_aperte_mutex));

      //attendo la terminazione del clock_casse, così da avere le informazioni sul tempo di apertura della cassa aggiornate
      if ((pthread_join(tid_clock_casse, NULL))!=0){
        fprintf(stderr, "Errore durante la join sul thread clock_casse: %d", id_cassa);
        exit(EXIT_FAILURE);
      }


      //stampo il resoconto della cassa
      lock_mutex_control(pthread_mutex_lock(&gestione_casse_mutex[id_cassa]));
      casse_supermarket[id_cassa]->t_medio_servizio = (casse_supermarket[id_cassa]->t_apertura/1000)/casse_supermarket[id_cassa]->n_clienti_Serviti;
      lock_mutex_control(pthread_mutex_lock(&logFile_mutex));
      fprintf(logFile, "| id Cassa: %d | n. prodotti elaborati: %d | n. di clienti: %d | tempo tot. di apertura: %0.3f | tempo medio di servizio: %0.3f | n. di chiusure: %d |\n", id_cassa, casse_supermarket[id_cassa]->n_prodotti_elaborati, casse_supermarket[id_cassa]->n_clienti_Serviti, casse_supermarket[id_cassa]->t_apertura/1000, casse_supermarket[id_cassa]->t_medio_servizio, casse_supermarket[id_cassa]->n_chiusure);
      fflush(logFile);
      unlock_mutex_control(pthread_mutex_unlock(&logFile_mutex));
      unlock_mutex_control(pthread_mutex_unlock(&gestione_casse_mutex[id_cassa]));
      return NULL;
    }
    else{
      unlock_mutex_control(pthread_mutex_unlock(&controllo_info_supermercato_mutex));
    }
    
  } 


  //ricevuta sigquit, svuoto la coda
  while (!isEmpty(queue_casse[id_cassa])){ 
    lock_mutex_control(pthread_mutex_lock(&queue_casse_mutex[id_cassa]));
    id_cliente = dequeue(queue_casse[id_cassa]);
    unlock_mutex_control(pthread_mutex_unlock(&queue_casse_mutex[id_cassa]));
    lock_mutex_control(pthread_mutex_lock(&info_clienti_mutex));
    lock_mutex_control(pthread_mutex_lock(&info_clienti[id_cliente]->in_queue));
    pthread_cond_signal(&info_clienti[id_cliente]->fine_queue); //risveglio il cliente in coda e lo faccio uscire dal supermercato
    unlock_mutex_control(pthread_mutex_unlock(&info_clienti[id_cliente]->in_queue));
    unlock_mutex_control(pthread_mutex_unlock(&info_clienti_mutex));
  }

  //se la cassa è aperta la chiudo
  lock_mutex_control(pthread_mutex_lock(&casse_aperte_mutex));
  if (binary_casse[id_cassa]==1){
    lock_mutex_control(pthread_mutex_lock(&controllo_apertura_mutex[id_cassa]));
    pthread_cond_signal(&cassa_chiusa_cond[id_cassa]); //avviso il thread di supporto clock_casse che la cassa è chiusa
    unlock_mutex_control(pthread_mutex_unlock(&controllo_apertura_mutex[id_cassa]));
    binary_casse[id_cassa]=0;
    casse_aperte--;
    unlock_mutex_control(pthread_mutex_unlock(&casse_aperte_mutex));
    lock_mutex_control(pthread_mutex_lock(&gestione_casse_mutex[id_cassa]));
    casse_supermarket[id_cassa]->cassa_aperta=0;
    unlock_mutex_control(pthread_mutex_unlock(&gestione_casse_mutex[id_cassa]));
  }
  else{
    unlock_mutex_control(pthread_mutex_unlock(&casse_aperte_mutex));
  }
  

  //attendo la terminazione del clock_casse, così da avere le informazioni sul tempo di apertura della cassa aggiornate
  if ((pthread_join(tid_clock_casse, NULL))!=0){
    fprintf(stderr, "Errore durante la join sul thread clock_casse: %d", id_cassa);
    exit(EXIT_FAILURE);
  }
  
  //calcolo il tempo medio di servizio e stampo
  lock_mutex_control(pthread_mutex_lock(&gestione_casse_mutex[id_cassa]));
  casse_supermarket[id_cassa]->t_medio_servizio = (casse_supermarket[id_cassa]->t_apertura/1000)/casse_supermarket[id_cassa]->n_clienti_Serviti;
  lock_mutex_control(pthread_mutex_lock(&logFile_mutex));
  fprintf(logFile, "| id Cassa: %d | n. prodotti elaborati: %d | n. di clienti: %d | tempo tot. di apertura: %0.3f | tempo medio di servizio: %0.3f | n. di chiusure: %d |\n", id_cassa, casse_supermarket[id_cassa]->n_prodotti_elaborati, casse_supermarket[id_cassa]->n_clienti_Serviti, casse_supermarket[id_cassa]->t_apertura/1000, casse_supermarket[id_cassa]->t_medio_servizio, casse_supermarket[id_cassa]->n_chiusure);
  fflush(logFile);
  unlock_mutex_control(pthread_mutex_unlock(&logFile_mutex));
  unlock_mutex_control(pthread_mutex_unlock(&gestione_casse_mutex[id_cassa]));
  //printf("THREAD GESTIONE CASSA: %d TERMINATO\n", id_cassa);
  return NULL;
}



//passerò l'id del cliente come argomento
void *GestioneCliente(void *arg){
  int id = (int) (__intptr_t) arg;
  //printf("ID CLIENTEEEEEEE %d\n", id);
  struct timespec timeEntry, timeExit, timeQueue, timeExitNoAcquisti;
  unsigned int seed;
  int no_prodotti = 0;
  int prodotti_acquistati = 0;
  ec_meno1(clock_gettime(CLOCK_REALTIME, &timeEntry), "Errore durante clock_gettime");  //acquisisco l'orario di entrata del cliente

  //creo e inizializzo il nuovo cliente
  lock_mutex_control(pthread_mutex_lock(&info_clienti_mutex));
  //printf("ALLOCO MEMORIA PER IL CLIENTE: %d\n", id);
  ec_null(info_clienti[id] = malloc(sizeof(cliente)), "Errore durante l'allocazione della memoria per il cliente");
  clienteInit(info_clienti[id], id);
  seed = id + time(NULL);
  info_clienti[id]->prodotti_acquistati = rand_r(&seed) % config->P;
  info_clienti[id]->t_acquisti = rand_r(&seed) %config->T + 10;
  prodotti_acquistati = info_clienti[id]->prodotti_acquistati;
  unlock_mutex_control(pthread_mutex_unlock(&info_clienti_mutex));
  //printf("ciao");
  

  lock_mutex_control(pthread_mutex_lock(&info_clienti_mutex));
  if (info_clienti[id]->prodotti_acquistati == 0){
    unlock_mutex_control(pthread_mutex_unlock(&info_clienti_mutex));
    no_prodotti=1;
    //gestione del cliente senza prodotti
    lock_mutex_control(pthread_mutex_lock(&queue_no_acquisti_mutex));
    lock_mutex_control(pthread_mutex_lock(&info_clienti[id]->in_queue));
    //printf("CLIENTE %d MESSO IN CODA", id);
    enqueue(queue_no_acquisti, id);
    unlock_mutex_control(pthread_mutex_unlock(&queue_no_acquisti_mutex));
    pthread_cond_wait(&info_clienti[id]->fine_queue, &info_clienti[id]->in_queue);
    unlock_mutex_control(pthread_mutex_unlock(&info_clienti[id]->in_queue));
    
    clock_gettime(CLOCK_REALTIME, &timeExitNoAcquisti);

    lock_mutex_control(pthread_mutex_lock(&info_clienti_mutex));
    lock_mutex_control(pthread_mutex_lock(&info_clienti[id]->in_queue));
    info_clienti[id]->t_tot_supermercato = (timeExitNoAcquisti.tv_sec - timeEntry.tv_sec)*1000 + ((timeExitNoAcquisti.tv_nsec - timeEntry.tv_nsec)/1000000);
    unlock_mutex_control(pthread_mutex_unlock(&info_clienti[id]->in_queue));
    unlock_mutex_control(pthread_mutex_unlock(&info_clienti_mutex));

  
    lock_mutex_control(pthread_mutex_lock(&controllo_info_supermercato_mutex));
    info_supermercato->clienti_nel_supermercato--;
    unlock_mutex_control(pthread_mutex_unlock(&controllo_info_supermercato_mutex));

    //TODO: mandare in stampa le info del cliente
    lock_mutex_control(pthread_mutex_lock(&info_clienti_mutex));
    lock_mutex_control(pthread_mutex_lock(&logFile_mutex));
    //printf("QUESTO È L'ID DEL CLIENTE IN STAMPA: %d\n", id);
    fprintf(logFile, "| id Cliente: %d | n prodotti acquistati: %d | tempo totale super: %0.3f | tempo tot speso in coda: %0.3f | n code visitate: %d |\n", id, info_clienti[id]->prodotti_acquistati, info_clienti[id]->t_tot_supermercato/1000, info_clienti[id]->t_coda/1000, info_clienti[id]->n_code_visitate);
    fflush(logFile);
    unlock_mutex_control(pthread_mutex_unlock(&logFile_mutex));
    unlock_mutex_control(pthread_mutex_unlock(&info_clienti_mutex));
    //printf("CLIENTE %d SENZA ACQUISTI USCITO: \n", id);
    return NULL;
  }
  if (no_prodotti==0){
    unlock_mutex_control(pthread_mutex_unlock(&info_clienti_mutex));
  }


  
  struct timespec res;
  lock_mutex_control(pthread_mutex_lock(&info_clienti_mutex));
  res.tv_sec = info_clienti[id]->t_acquisti/1000;
  res.tv_nsec = (info_clienti[id]->t_acquisti%1000)*1000000;
  //printf("t_acquisti: %d\n", info_clienti[id]->t_acquisti);
  unlock_mutex_control(pthread_mutex_unlock(&info_clienti_mutex));
  
  nanosleep(&res, NULL);
  int id_cassa;
  int cassa_trovata = 0;

  do
  {
    id_cassa = rand_r(&seed) % (config->K);
    
    lock_mutex_control(pthread_mutex_lock(&casse_aperte_mutex));
    if (binary_casse[id_cassa]==1) { cassa_trovata=1; }//printf("CASSA SCELTAAAAAAAAAAAAAAAAAAA: %d\n", id_cassa);
    unlock_mutex_control(pthread_mutex_unlock(&casse_aperte_mutex));
    //se il supermercato chiude, il cliente smette di cercare una cassa libera
    if (exitquit==1){
      break;
    }
    if(exithup==1 && cassa_trovata==0){
      printf("RICERCA CASSA APERTA DOPO SIGHUP\n");
      id_cassa=0;
      while (cassa_trovata==0){
        lock_mutex_control(pthread_mutex_lock(&casse_aperte_mutex));
        if (binary_casse[id_cassa]==1){
          unlock_mutex_control(pthread_mutex_unlock(&casse_aperte_mutex));
          cassa_trovata=1;
          printf("CASSA FINALE TROVATAAAAAAAAAA\n");
        } else{
          unlock_mutex_control(pthread_mutex_unlock(&casse_aperte_mutex));
          id_cassa++;
        }
      }
    }
  }while (cassa_trovata==0);

  if (exitquit==0){
    
    lock_mutex_control(pthread_mutex_lock(&queue_casse_mutex[id_cassa]));
    //printf("thread: %ld CLIENTE %d MESSO IN CODA NELLA CASSA %d\n", pthread_self(), id, id_cassa);
    enqueue(queue_casse[id_cassa], id);
    unlock_mutex_control(pthread_mutex_unlock(&queue_casse_mutex[id_cassa]));
    //salvo il tempo di ingresso in coda
    ec_meno1(clock_gettime(CLOCK_REALTIME, &timeQueue), "Errore durante la clock_gettime del cliente in coda");
    

    lock_mutex_control(pthread_mutex_lock(&info_clienti_mutex));
    lock_mutex_control(pthread_mutex_lock(&info_clienti[id]->in_queue));
    info_clienti[id]->n_code_visitate++;
    unlock_mutex_control(pthread_mutex_unlock(&info_clienti[id]->in_queue));
    unlock_mutex_control(pthread_mutex_unlock(&info_clienti_mutex));

    lock_mutex_control(pthread_mutex_lock(&info_clienti[id]->in_queue));
    pthread_cond_wait(&info_clienti[id]->fine_queue, &info_clienti[id]->in_queue);
    unlock_mutex_control(pthread_mutex_unlock(&info_clienti[id]->in_queue));

    //salvo il tempo di fine gestione del cliente
    ec_meno1(clock_gettime(CLOCK_REALTIME, &timeExit), "Errore durante la clock_gettime di timeQueueExit");
    lock_mutex_control(pthread_mutex_lock(&info_clienti_mutex));
    lock_mutex_control(pthread_mutex_lock(&info_clienti[id]->in_queue));
    info_clienti[id]->t_coda = (((timeExit.tv_sec - timeQueue.tv_sec)*1000) + ((timeExit.tv_nsec-timeQueue.tv_nsec)/1000000)) - info_clienti[id]->t_gestione;
    unlock_mutex_control(pthread_mutex_unlock(&info_clienti[id]->in_queue));
    unlock_mutex_control(pthread_mutex_unlock(&info_clienti_mutex));

  }
  //salvo il tempo di fine gestione del cliente
  ec_meno1(clock_gettime(CLOCK_REALTIME, &timeExit), "Errore durante la clock_gettime di timeQueueExit");
  lock_mutex_control(pthread_mutex_lock(&info_clienti_mutex));
  lock_mutex_control(pthread_mutex_lock(&info_clienti[id]->in_queue));
  info_clienti[id]->t_tot_supermercato = (timeExit.tv_sec - timeEntry.tv_sec)*1000 + ((timeExit.tv_nsec - timeEntry.tv_nsec)/1000000);
  unlock_mutex_control(pthread_mutex_unlock(&info_clienti[id]->in_queue));
  unlock_mutex_control(pthread_mutex_unlock(&info_clienti_mutex));
  
  

  //TODO: stampo le informazioni del cliente, aggiornare anche le informazioni del supermercato
  
  lock_mutex_control(pthread_mutex_lock(&info_clienti_mutex));
  lock_mutex_control(pthread_mutex_lock(&logFile_mutex));
  //printf("QUESTO È L'ID DEL CLIENTE IN STAMPA: %d\n", id);
  fprintf(logFile, "| id Cliente: %d | n prodotti acquistati: %d | tempo totale super: %0.3f | tempo tot speso in coda: %0.3f | n code visitate: %d | id cassa usata: %d |\n", id, info_clienti[id]->prodotti_acquistati, info_clienti[id]->t_tot_supermercato/1000, info_clienti[id]->t_coda/1000, info_clienti[id]->n_code_visitate, info_clienti[id]->id_cassa_usata);
  fflush(logFile);
  unlock_mutex_control(pthread_mutex_unlock(&logFile_mutex));
  unlock_mutex_control(pthread_mutex_unlock(&info_clienti_mutex));
  

  lock_mutex_control(pthread_mutex_lock(&controllo_info_supermercato_mutex));
  info_supermercato->clienti_nel_supermercato--;
  info_supermercato->n_prodotti_acquistati+=prodotti_acquistati;
  //printf("CLIENTI NEL SUPERMERCATO: %d\n", info_supermercato->clienti_nel_supermercato);
  info_supermercato->clienti_serviti++;
  unlock_mutex_control(pthread_mutex_unlock(&controllo_info_supermercato_mutex));
  //printf("CLIENTE %d USCITO: \n", id);
  return NULL;
}


int main(int argc, char *argv[]){

  if (argc != 2){
    printf("Troppi pochi argomenti\n");
    return -1;
  }



  struct sigaction close;
  config = malloc(sizeof(file));
  ec_null(config, "malloc su config non riuscita");


  leggiFile(config, argv[1]);


  if ((logFile = fopen("logFile.log", "w")) == NULL){
    fprintf(stderr, "Errore durante l'apertura del file logFile");
    exit(EXIT_FAILURE);
  }
  

  memset(&close, 0, sizeof(close));
  close.sa_handler = gestore;
  ec_meno1(sigaction(SIGHUP, &close, NULL), "Errore durante la gestione di sighup");
  ec_meno1(sigaction(SIGQUIT, &close, NULL), "Errore durante la gestione di sigquit");


  //inizializzo il direttore
  pthread_t tid_direttore;
  int err;

  if ((err = pthread_create(&tid_direttore, NULL, Direttore, NULL))!=0){
    fprintf(stderr, "errore durante la creazione del thread direttore");
    exit(EXIT_FAILURE);
  }

  if ((err = pthread_join(tid_direttore, NULL))!=0){
    fprintf(stderr, "Errore durante la join nel thread direttore");
    exit(EXIT_FAILURE);
  }

  /*
  while (1){
    lock_mutex_control(pthread_mutex_lock(&controllo_info_supermercato_mutex));
    if (info_supermercato->clienti_nel_supermercato==0){
      unlock_mutex_control(pthread_mutex_unlock(&controllo_info_supermercato_mutex));
      break;
    }
    else{
      info_supermercato->clienti_nel_supermercato--;
      unlock_mutex_control(pthread_mutex_unlock(&controllo_info_supermercato_mutex));
    }
  }*/
  
  
  lock_mutex_control(pthread_mutex_lock(&controllo_info_supermercato_mutex));
  lock_mutex_control(pthread_mutex_lock(&logFile_mutex));
  fprintf(logFile, "| STATISTICHE SUPERMERCATO: | n. clienti serviti: %d | n. prodotti acquistati: %d |*", info_supermercato->clienti_serviti, info_supermercato->n_prodotti_acquistati);
  unlock_mutex_control(pthread_mutex_unlock(&logFile_mutex));
  unlock_mutex_control(pthread_mutex_unlock(&controllo_info_supermercato_mutex));

  
  //libero la memoria occupata
  free(controllo_apertura_mutex); 
  free(cassa_chiusa_cond);  
  free(gestione_casse_mutex); 
  free(queue_no_acquisti);
  free(queue_casse_mutex);
  free(binary_casse); 
  
  
  for(size_t i = 0; i<config->K; i++){
    free(queue_casse[i]);
    free(casse_supermarket[i]);
  }

  for(size_t i = 0; i<info_supermercato->clienti_entrati; i++){
    free(info_clienti[i]);
  }  

  free(queue_casse);
  free(casse_supermarket);
  free(info_supermercato); 
  free(info_clienti);  
  free(config);
  
  lock_mutex_control(pthread_mutex_lock(&logFile_mutex));
  fclose(logFile); 
  unlock_mutex_control(pthread_mutex_unlock(&logFile_mutex));
  printf("PROGRAMMA TERMINATO\n");
  exit(EXIT_SUCCESS);
}



void leggiFile(file* config, char* arg){
  FILE *ifp;
  ifp = fopen(arg, "r");

  controllo_file(fscanf(ifp, "%d", &config->K), "Errore durante la lettura di K");
  controllo_file(fscanf(ifp, "%d", &config->C), "Errore durante la lettura di C");
  controllo_file(fscanf(ifp, "%d", &config->E), "Errore durante la lettura di E");
  controllo_file(fscanf(ifp, "%d", &config->T), "Errore durante la lettura di T");
  controllo_file(fscanf(ifp, "%d", &config->P), "Errore durante la lettura di P");
  controllo_file(fscanf(ifp, "%d", &config->S), "Errore durante la lettura di S");
  controllo_file(fscanf(ifp, "%d", &config->S1), "Errore durante la lettura di S1");
  controllo_file(fscanf(ifp, "%d", &config->S2), "Errore durante la lettura di S2");
  controllo_file(fscanf(ifp, "%d", &config->casse_iniziali), "Errore durante la lettura di casse_iniziali");
  controllo_file(fscanf(ifp, "%d", &config->clienti_iniziali), "Errore durante la lettura di clienti_iniziali");
  controllo_file(fscanf(ifp, "%d", &config->t_fisso_prodotto), "Errore durante la lettura di t_fisso_prodotto");
  controllo_file(fscanf(ifp, "%d", &config->t_info_direttore), "Errore durante la lettura di t_info_direttore");
  fflush(ifp);
  fclose(ifp);


  //controllo che i valori inseriti rispettino le specifiche

  if (config->casse_iniziali > config->K){
    fprintf(stderr, "Le casse iniziali devono essere inferiori a quelle totali");
    fflush(stderr);
    exit(EXIT_FAILURE);
  }

  if (config->clienti_iniziali > config->C){
    fprintf(stderr, "I clienti iniziali devono essere minori dei clienti totali");
    fflush(stderr);
    exit(EXIT_FAILURE);
  }

  if (config->E > config->C){
    fprintf(stderr, "il valore E deve essere minore di C");
    fflush(stderr);
    exit(EXIT_FAILURE);
  }
}


void clienteInit(struct cliente *cl, int id){
  cl->id = id;
  cl->n_code_visitate = 0;
  cl->prodotti_acquistati = 0;
  cl->t_acquisti = 0;
  cl->t_coda = 0;
  cl->t_tot_supermercato = 0;
  cl->t_gestione = 0;
  cl->id_cassa_usata = 0;
  if((pthread_mutex_init(&cl->in_queue, NULL))!=0){
    fprintf(stderr, "errore durante l'inizializzazione della mutex");
    exit(EXIT_FAILURE);
  }
  if((pthread_cond_init(&cl->fine_queue, NULL))!=0){
    fprintf(stderr, "errore durante l'inizializzazione della condizione");
    exit(EXIT_FAILURE);
  }
  
}


void cassaInit(struct cassa *ca, int id){
  unsigned int seed = time(NULL) + id;
  ca->id = id;
  ca->n_chiusure = 0;
  ca->n_clienti_Serviti = 0;
  ca->n_prodotti_elaborati = 0;
  ca->t_apertura = 0;
  ca->t_medio_servizio = 0;
  ca->cassa_aperta = 0;
  do{
    ca->t_fisso_cassiere = rand_r(&seed) % 80 + 20;
  } while (ca->t_fisso_cassiere>80);
  
   
}


//funzione di supporto per spostare i clienti da una cassa all'altra
void spostaClienti(struct Queue* q, int id_cassa){
  int i = 0;
  lock_mutex_control(pthread_mutex_lock(&casse_aperte_mutex));
  while (binary_casse[i]!=1 && i<config->K){
    i++;
  }
  unlock_mutex_control(pthread_mutex_unlock(&casse_aperte_mutex));

  int tmp;
  lock_mutex_control(pthread_mutex_lock(&casse_aperte_mutex));
  if (binary_casse[i] == 1){
    unlock_mutex_control(pthread_mutex_unlock(&casse_aperte_mutex));
    while (!(isEmpty(q))){
      lock_mutex_control(pthread_mutex_lock(&queue_casse_mutex[id_cassa]));
      tmp = dequeue(q);
      unlock_mutex_control(pthread_mutex_unlock(&queue_casse_mutex[id_cassa]));
      lock_mutex_control(pthread_mutex_lock(&info_clienti_mutex));
      //printf("SONO QUAAAAAAAAAAAAAAAAAAAAAAAAA\n");
      info_clienti[tmp]->n_code_visitate++;
      unlock_mutex_control(pthread_mutex_unlock(&info_clienti_mutex));
      lock_mutex_control(pthread_mutex_lock(&queue_casse_mutex[i]));
      enqueue(queue_casse[i], tmp);
      unlock_mutex_control(pthread_mutex_unlock(&queue_casse_mutex[i]));
    }
  }
  else{
    unlock_mutex_control(pthread_mutex_unlock(&casse_aperte_mutex));
  }
  
}

/*
void logPrintCliente(int id){
  lock_mutex_control(pthread_mutex_lock(&info_clienti_mutex));
  printf("QUESTO È L'ID DEL CLIENTE IN STAMPA: %d\n", id);
  fprintf(logFile, "| id Cliente: %d | n prodotti acquistati: %d | tempo totale super: %0.3f | tempo tot speso in coda: %0.3f | n code visitate: %d | id cassa usata: %d |\n", id, info_clienti[id]->prodotti_acquistati, info_clienti[id]->t_tot_supermercato/1000, info_clienti[id]->t_coda/1000, info_clienti[id]->n_code_visitate, info_clienti[id]->id_cassa_usata);
  unlock_mutex_control(pthread_mutex_unlock(&info_clienti_mutex));
}*/