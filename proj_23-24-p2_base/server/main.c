#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>

#include "common/constants.h"
#include "common/io.h"
#include "operations.h"

typedef struct {
    char request_pipe[MAX_PIPE_NAME_SIZE], response_pipe[MAX_PIPE_NAME_SIZE];
} Session;

Session buffer[MAX_SESSION_COUNT];
int prod_ptr = 0, cons_ptr = 0, sessions = 0;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER; 
pthread_mutex_t stdout_mutex = PTHREAD_MUTEX_INITIALIZER; 
pthread_cond_t canCons, canProd;

int sigint_flag = 0;

void SIGUSR1_handler() {
  sigint_flag = 1;
}

void *process_client(void *arg){ 
  int thread_id = *((int*) arg);

  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR1);

  pthread_mutex_lock(&stdout_mutex);
  if (pthread_sigmask(SIG_BLOCK, &mask, NULL) != 0) {
    fprintf(stderr, "Error setting up sigmask.\n");
  }
  pthread_mutex_unlock(&stdout_mutex);

  while (1){
    Session client_session;
    int fd_req, fd_resp;
    pthread_mutex_lock(&mutex);

    while (sessions == 0) 
      pthread_cond_wait(&canCons, &mutex);

    client_session = buffer[cons_ptr++];
    if (cons_ptr == MAX_SESSION_COUNT) 
      cons_ptr = 0; 

    sessions--; 
    pthread_cond_signal(&canProd);
    pthread_mutex_unlock(&mutex);

    pthread_mutex_lock(&stdout_mutex);
    if ((fd_req = open(client_session.request_pipe, O_RDONLY)) < 0) 
      fprintf(stderr, "Error opening requests pipe.\n");
      
    if ((fd_resp = open(client_session.response_pipe, O_WRONLY)) < 0) 
      fprintf(stderr, "Error opening responses pipe.\n");
    pthread_mutex_unlock(&stdout_mutex);
      
    write(fd_resp, &thread_id, sizeof(int));

    int client_active = 1;
    while (client_active){
      char OP_CODE;
      int ret, client_id;
      unsigned int event_id;
      size_t num_rows, num_cols, num_seats;
      size_t xs[MAX_RESERVATION_SIZE], ys[MAX_RESERVATION_SIZE];

      // client terminates during process
      if(read(fd_req, &OP_CODE, sizeof(char)) == 0){
        client_active = 0;
        break;
      }
      read(fd_req, &client_id, sizeof(int));
      if (client_id == thread_id){
        switch(OP_CODE){
          case '2':
            close(fd_req);
            close(fd_resp);
            client_active = 0;
            break;

          case '3':
            read(fd_req, &event_id, sizeof(unsigned int));
            read(fd_req, &num_rows, sizeof(size_t));
            read(fd_req, &num_cols, sizeof(size_t));
            ret = ems_create(event_id, num_rows, num_cols);
            if(write(fd_resp, &ret, sizeof(int)) == -1 && errno == EPIPE)
              client_active = 0;
            break;

          case '4':
            read(fd_req, &event_id, sizeof(unsigned int));
            read(fd_req, &num_seats, sizeof(size_t));
            read(fd_req, xs, sizeof(size_t)*num_seats);
            read(fd_req, ys, sizeof(size_t)*num_seats);
            ret = ems_reserve(event_id, num_seats, xs, ys);
            if(write(fd_resp, &ret, sizeof(int)) == -1 && errno == EPIPE)
              client_active = 0;
            break;

          case '5':
            read(fd_req, &event_id, sizeof(unsigned int));
            ems_show(fd_resp, event_id);
            break;

          case '6':
            ems_list_events(fd_resp);
            break;
        }
      }
    }
  }
}

int main(int argc, char* argv[]) {
  if (argc < 2 || argc > 3) {
    fprintf(stderr, "Usage: %s\n <pipe_path> [delay]\n", argv[0]);
    return 1;
  }

  char* endptr;
  unsigned int state_access_delay_us = STATE_ACCESS_DELAY_US;
  if (argc == 3) {
    unsigned long int delay = strtoul(argv[2], &endptr, 10);

    if (*endptr != '\0' || delay > UINT_MAX) {
      fprintf(stderr, "Invalid delay value or value too large\n");
      return 1;
    }

    state_access_delay_us = (unsigned int)delay;
  }

  if (ems_init(state_access_delay_us)) {
    fprintf(stderr, "Failed to initialize EMS\n");
    return 1;
  }

  pthread_t thread_array[MAX_SESSION_COUNT];
  for (int i = 0; i < MAX_SESSION_COUNT; i++){
    int *arg = malloc(sizeof(int));
    *arg = i;
    if (pthread_create(&thread_array[i], NULL, process_client, arg) != 0) {
      fprintf(stderr, "Error creating thread\n");
      return 1;
    }
  }

  const char* pipe_path = argv[1];
  unlink(pipe_path);
  if (mkfifo(pipe_path, 0666) == -1){
    fprintf(stderr, "Error creating pipe.\n");
    return 1;
  }

  signal(SIGUSR1, SIGUSR1_handler);
  signal(SIGPIPE, SIG_IGN);
  
  while (1) {
    int fd_serv;
    char request_pipe[MAX_PIPE_NAME_SIZE], response_pipe[MAX_PIPE_NAME_SIZE];
    memset(request_pipe, 0, MAX_PIPE_NAME_SIZE);
    memset(response_pipe, 0, MAX_PIPE_NAME_SIZE);
    char OP_CODE;

    if ((fd_serv = open(pipe_path, O_RDONLY)) < 0 && errno != EINTR) 
      fprintf(stderr, "Error opening pipe.\n");

    if (sigint_flag){
      sigint_flag = 0;
      pthread_mutex_lock(&stdout_mutex);
      if (ems_show_all_events()) 
        fprintf(stderr, "Failed to show all events\n");
      pthread_mutex_unlock(&stdout_mutex);
      close(fd_serv);
      continue;
    }

    if(read(fd_serv, &OP_CODE, sizeof(char)) < 1)
      fprintf(stderr, "Error reading from server pipe.\n");

    if(read(fd_serv, request_pipe, sizeof(char) * MAX_PIPE_NAME_SIZE) < MAX_PIPE_NAME_SIZE)
      fprintf(stderr, "Error reading from server pipe.\n");

    if(read(fd_serv, response_pipe, sizeof(char) * MAX_PIPE_NAME_SIZE) < MAX_PIPE_NAME_SIZE)
      fprintf(stderr, "Error reading from server pipe.\n");

    Session client_session;
    memcpy(client_session.request_pipe, request_pipe, sizeof(char)*MAX_PIPE_NAME_SIZE);
    memcpy(client_session.response_pipe, response_pipe, sizeof(char)*MAX_PIPE_NAME_SIZE);

    pthread_mutex_lock(&mutex);

    while (sessions == MAX_SESSION_COUNT) 
      pthread_cond_wait(&canProd,&mutex);

    buffer[prod_ptr++] = client_session; 
    if(prod_ptr == MAX_SESSION_COUNT) 
      prod_ptr = 0;
    sessions++;
    pthread_cond_signal(&canCons);
    pthread_mutex_unlock(&mutex);

    close(fd_serv);
  }
}