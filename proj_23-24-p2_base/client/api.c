#include "api.h"
#include "../common/constants.h"
#include "../common/io.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int fd_req, fd_resp;
const char* req_path;
const char* resp_path;
int session_id;

int ems_setup(char const* req_pipe_path, char const* resp_pipe_path, char const* server_pipe_path) {

  char msg[(MAX_PIPE_NAME_SIZE*2 + 1) * sizeof(char)];
  int fd_serv;
  req_path = req_pipe_path;
  resp_path = resp_pipe_path;
  unlink(req_pipe_path);
  unlink(resp_pipe_path);

  if (mkfifo(req_pipe_path, 0666) == -1){
    fprintf(stderr, "Error creating request pipe.\n");
    return 1;
  }

  if (mkfifo(resp_pipe_path, 0666) == -1){
    fprintf(stderr, "Error creating response pipe.\n");
    return 1;
  }

  if ((fd_serv = open(server_pipe_path, O_WRONLY)) < 0) {
    fprintf(stderr, "Error opening server pipe.\n");
    return 1;
  }

  char OP_CODE = '1';
  memset(msg, 0, MAX_PIPE_NAME_SIZE*2 + 1);
  memcpy(msg, &OP_CODE, sizeof(char));
  memcpy(msg + sizeof(char), req_pipe_path, strlen(req_pipe_path));
  memcpy(msg + sizeof(char)*(MAX_PIPE_NAME_SIZE+1), resp_pipe_path, strlen(resp_pipe_path));
  write(fd_serv, msg, (MAX_PIPE_NAME_SIZE*2 + 1) * sizeof(char));
  close(fd_serv);

  if ((fd_req = open(req_pipe_path, O_WRONLY)) < 0) {
    fprintf(stderr, "Error opening requests pipe.\n");
    return 1;
  }
  if ((fd_resp = open(resp_pipe_path, O_RDONLY)) < 0) {
    fprintf(stderr, "Error opening responses pipe.\n");
    return 1;
  }

  read(fd_resp, &session_id, sizeof(int));
  return 0;
}

int ems_quit(void) { 
  
  char OP_CODE = '2';
  char msg[sizeof(char) + sizeof(int)];
  memcpy(msg, &OP_CODE, sizeof(char));
  memcpy(msg + sizeof(char), &session_id, sizeof(int));
  write(fd_req, msg, sizeof(char) + sizeof(int));
  close(fd_req);
  close(fd_resp);
  return 0;
}

int ems_create(unsigned int event_id, size_t num_rows, size_t num_cols) {

  char OP_CODE = '3';
  int ret;
  char msg[sizeof(char) + sizeof(int) + sizeof(unsigned int) + sizeof(size_t) * 2];

  memcpy(msg, &OP_CODE, sizeof(char));
  memcpy(msg + sizeof(char), &session_id, sizeof(int));
  memcpy(msg + sizeof(char) + sizeof(int), &event_id, sizeof(unsigned int));
  memcpy(msg + sizeof(char) + sizeof(int) + sizeof(unsigned int), &num_rows, sizeof(size_t));
  memcpy(msg + sizeof(char) + sizeof(int) + sizeof(unsigned int) + sizeof(size_t), &num_cols, sizeof(size_t));
  write(fd_req, msg, sizeof(char) + sizeof(int) + sizeof(unsigned int) + sizeof(size_t) * 2);
  read(fd_resp, &ret, sizeof(int));

  return ret;
}

int ems_reserve(unsigned int event_id, size_t num_seats, size_t* xs, size_t* ys) {

  char OP_CODE = '4';
  int ret;
  char msg[sizeof(char) + sizeof(int) + sizeof(unsigned int) + sizeof(size_t)*(num_seats*2+1)];

  memcpy(msg, &OP_CODE, sizeof(char));
  memcpy(msg + sizeof(char), &session_id, sizeof(int));
  memcpy(msg + sizeof(char) + sizeof(int), &event_id, sizeof(unsigned int));
  memcpy(msg + sizeof(char) + sizeof(int) + sizeof(unsigned int), &num_seats, sizeof(size_t));
  memcpy(msg + sizeof(char) + sizeof(int) + sizeof(unsigned int) + sizeof(size_t), xs, sizeof(size_t)*num_seats);
  memcpy(msg + sizeof(char) + sizeof(int) + sizeof(unsigned int) + sizeof(size_t)*(num_seats+1), ys, sizeof(size_t)*num_seats);
  write(fd_req, msg, sizeof(char) + sizeof(int) + sizeof(unsigned int) + sizeof(size_t)*(num_seats*2+1));
  read(fd_resp, &ret, sizeof(int));

  return ret;
}

int ems_show(int out_fd, unsigned int event_id) {
  char OP_CODE = '5';
  int ret;
  size_t num_rows, num_cols;
  char msg[sizeof(char) + sizeof(int) + sizeof(unsigned int)];
  

  memcpy(msg, &OP_CODE, sizeof(char));
  memcpy(msg + sizeof(char), &session_id, sizeof(int));
  memcpy(msg + sizeof(char) + sizeof(int), &event_id, sizeof(unsigned int));
  write(fd_req, msg, sizeof(char) + sizeof(int) + sizeof(unsigned int));

  read(fd_resp, &ret, sizeof(int));
  if (ret != 0)
    return ret;

  read(fd_resp, &num_rows, sizeof(size_t));
  read(fd_resp, &num_cols, sizeof(size_t));

  unsigned int seats[num_rows * num_cols];
  read(fd_resp, seats, num_rows*num_cols*sizeof(unsigned int));

  for (size_t i = 0; i<num_rows; i++){
    for (size_t j = 0; j<num_cols; j++){
      char buffer[16];
      sprintf(buffer, "%u", seats[i*num_cols + j]);
      print_str(out_fd, buffer);
      if (j<num_cols-1)
        print_str(out_fd, " ");
    }
    print_str(out_fd, "\n");
  }
  return ret;
}

int ems_list_events(int out_fd) {

  char OP_CODE = '6';
  char msg[sizeof(char) + sizeof(int)];
  int ret;
  size_t num_events;

  memcpy(msg, &OP_CODE, sizeof(char));
  memcpy(msg + sizeof(char), &session_id, sizeof(int));
  write(fd_req, msg, sizeof(char) + sizeof(int));

  read(fd_resp, &ret, sizeof(int));
  if (ret != 0)
    return ret;

  read(fd_resp, &num_events, sizeof(size_t));
  if (!num_events)
    print_str(out_fd, "No events\n");
  else{
    unsigned int event_ids[num_events];
    read(fd_resp, event_ids, sizeof(unsigned int)*num_events);
    for (size_t i = 0; i<num_events; i++){
      char buffer[16];
      sprintf(buffer, "Event: %u\n", event_ids[i]);
      print_str(out_fd, buffer);
    }
  }

  return ret;
}
