#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "common/constants.h"
#include "common/io.h"
#include "operations.h"

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

  //TODO: Intialize server, create worker threads
  const char* pipe_path = argv[1];
  int fd_serv, fd_resp, fd_req;
  unlink(pipe_path);
  if (mkfifo(pipe_path, 0666) == -1){
    fprintf(stderr, "Error creating pipe.\n");
    return 1;
  }

  if ((fd_serv = open(pipe_path, O_RDONLY)) < 0) {
    fprintf(stderr, "Error opening pipe.\n");
    return 1;
  }

  char request_pipe[MAX_PIPE_NAME_SIZE], response_pipe[MAX_PIPE_NAME_SIZE];
  memset(request_pipe, 0, MAX_PIPE_NAME_SIZE);
  memset(response_pipe, 0, MAX_PIPE_NAME_SIZE);
  char OP_CODE;
  int session_id = 0;

  while (1) {
    read(fd_serv, &OP_CODE, sizeof(char));
    read(fd_serv, request_pipe, sizeof(char)*MAX_PIPE_NAME_SIZE);
    read(fd_serv, response_pipe, sizeof(char)*MAX_PIPE_NAME_SIZE);
    if (OP_CODE == '1' && session_id < MAX_SESSION_COUNT){
      if ((fd_req = open(request_pipe, O_RDONLY)) < 0) {
        fprintf(stderr, "Error opening requests pipe.\n");
        return 1;
      }
      if ((fd_resp = open(response_pipe, O_WRONLY)) < 0) {
        fprintf(stderr, "Error opening responses pipe.\n");
        return 1;
      }
      write(fd_resp, &session_id, sizeof(int));
      session_id++;
    }
    break;
    
    //TODO: Read from pipe
    //TODO: Write new client to the producer-consumer buffer
  }

  int client_active = 1;

  while (client_active){
    char ch;
    int ret;
    unsigned int event_id;
    size_t num_rows, num_cols, num_seats;
    size_t xs[MAX_RESERVATION_SIZE], ys[MAX_RESERVATION_SIZE];

    read(fd_req, &ch, sizeof(char));
    switch(ch){
      case '2':
        client_active = 0;
        break;

      case '3':
        read(fd_req, &event_id, sizeof(unsigned int));
        read(fd_req, &num_rows, sizeof(size_t));
        read(fd_req, &num_cols, sizeof(size_t));
        ret = ems_create(event_id, num_rows, num_cols);
        write(fd_resp, &ret, sizeof(int));
        break;

      case '4':
        read(fd_req, &event_id, sizeof(unsigned int));
        read(fd_req, &num_seats, sizeof(size_t));
        read(fd_req, xs, sizeof(size_t)*num_seats);
        read(fd_req, ys, sizeof(size_t)*num_seats);
        ret = ems_reserve(event_id, num_seats, xs, ys);
        write(fd_resp, &ret, sizeof(int));
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

  //TODO: Close Server
  close(fd_serv);
  ems_terminate();
}