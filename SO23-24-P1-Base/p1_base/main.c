#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pthread.h>

#include "constants.h"
#include "operations.h"
#include "parser.h"

struct CommandInfo {
    int inputFd;
    int outputFd;
    unsigned int thread_id;
    unsigned int event_id;
    size_t num_rows;
    size_t num_columns;
    size_t num_coords;
    size_t xs[MAX_RESERVATION_SIZE];
    size_t ys[MAX_RESERVATION_SIZE];
    unsigned int delay;
};

pthread_mutex_t mutex_in = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t stop_mutex = PTHREAD_MUTEX_INITIALIZER;
unsigned int thread_stop = 0;
unsigned int stop_time = 0;
int barrier = 0;

void *process_command(void *arg) {
  struct CommandInfo *cmd_info = (struct CommandInfo *)arg;

  unsigned int thread_id;
  int eof = 0;
  while(!eof){
    pthread_mutex_lock(&stop_mutex);
    if (thread_stop == cmd_info->thread_id){
      printf("Waiting...\n");
      ems_wait(stop_time);
      thread_stop = 0;
    }
    if (barrier == 1){
      pthread_mutex_unlock(&stop_mutex);
      pthread_exit((void*)1);
    }
    pthread_mutex_unlock(&stop_mutex);
    pthread_mutex_lock(&mutex_in);
    switch (get_next(cmd_info->inputFd)) {
      case CMD_CREATE:
          if (parse_create(cmd_info->inputFd, &cmd_info->event_id, &cmd_info->num_rows, &cmd_info->num_columns) != 0) {
              fprintf(stderr, "Invalid command. See HELP for usage\n");
              pthread_mutex_unlock(&mutex_in);
              break;
          }
          pthread_mutex_unlock(&mutex_in);
          if (ems_create(cmd_info->event_id, cmd_info->num_rows, cmd_info->num_columns)) {
              fprintf(stderr, "Failed to create event\n");
          }

          break;

      case CMD_RESERVE:
          cmd_info->num_coords = parse_reserve(cmd_info->inputFd, MAX_RESERVATION_SIZE, 
                                    &cmd_info->event_id, cmd_info->xs, cmd_info->ys);

          if (cmd_info->num_coords == 0) {
              fprintf(stderr, "Invalid command. See HELP for usage\n");
              pthread_mutex_unlock(&mutex_in);
              break;
          }
          pthread_mutex_unlock(&mutex_in);
          if (ems_reserve(cmd_info->event_id, cmd_info->num_coords, cmd_info->xs, cmd_info->ys)) {
              fprintf(stderr, "Failed to reserve seats\n");
          }

          break;

      case CMD_SHOW:
          if (parse_show(cmd_info->inputFd, &cmd_info->event_id) != 0) {
              pthread_mutex_unlock(&mutex_in);
              fprintf(stderr, "Invalid command. See HELP for usage\n");
          } 
          else {
            pthread_mutex_unlock(&mutex_in);
            if (ems_show(cmd_info->event_id, cmd_info->outputFd)) {
                fprintf(stderr, "Failed to show event\n");
            }
          }
          break;

      case CMD_LIST_EVENTS:
          pthread_mutex_unlock(&mutex_in);
          if (ems_list_events(cmd_info->outputFd)) {
              fprintf(stderr, "Failed to list events\n");
          }
          break;

      case CMD_WAIT:
          if (parse_wait(cmd_info->inputFd, &cmd_info->delay, &thread_id) == -1) {
              pthread_mutex_unlock(&mutex_in);
              fprintf(stderr, "Invalid command. See HELP for usage\n");
          } 
          else if (cmd_info->delay > 0) {
              if (thread_id == 0) {
                  printf("Waiting...\n");
                  ems_wait(cmd_info->delay);
                  pthread_mutex_unlock(&mutex_in);
              }
              else{
                pthread_mutex_lock(&stop_mutex);
                thread_stop = thread_id;
                stop_time = cmd_info->delay;
                pthread_mutex_unlock(&stop_mutex);
              }
              pthread_mutex_unlock(&mutex_in);
          }
          else{
            pthread_mutex_unlock(&mutex_in);
          }
          break;

      case CMD_INVALID:
          pthread_mutex_unlock(&mutex_in);
          fprintf(stderr, "Invalid command. See HELP for usage\n");
          break;

      case CMD_HELP:
          pthread_mutex_unlock(&mutex_in);
          printf(
              "Available commands:\n"
              "  CREATE <event_id> <num_rows> <num_columns>\n"
              "  RESERVE <event_id> [(<x1>,<y1>) (<x2>,<y2>) ...]\n"
              "  SHOW <event_id>\n"
              "  LIST\n"
              "  WAIT <delay_ms> [thread_id]\n"  // thread_id is not implemented
              "  BARRIER\n"                      // Not implemented
              "  HELP\n");
          break;

      case CMD_BARRIER:
          pthread_mutex_unlock(&mutex_in);
          break;
      case CMD_EMPTY:
          pthread_mutex_unlock(&mutex_in);
          break;

      case EOC:
          pthread_mutex_unlock(&mutex_in);
          eof = 1;
    }
  }
  pthread_exit((void*)0);
  return NULL;
}

int main(int argc, char *argv[]) {
  unsigned int state_access_delay_ms = STATE_ACCESS_DELAY_MS;
  int MAX_PROC, MAX_THREADS, n_proc;
  

  n_proc = 0;
  MAX_PROC = atoi(argv[2]);
  MAX_THREADS = atoi(argv[3]);

  if (argc > 4) {
    char *endptr;
    unsigned long int delay = strtoul(argv[4], &endptr, 10);

    if (*endptr != '\0' || delay > UINT_MAX) {
      fprintf(stderr, "Invalid delay value or value too large\n");
      return 1;
    }

    state_access_delay_ms = (unsigned int)delay;
  }

  if (ems_init(state_access_delay_ms)) {
    fprintf(stderr, "Failed to initialize EMS\n");
    return 1;
  }

  DIR* dirp;
  struct dirent *dp;
  char *dirpath = argv[1];
  if ((dirp = opendir(dirpath)) == NULL){
    fprintf(stderr, "Error opening directory.\n");
    return -1;
  }

  int dp_n = 0;
  pthread_t thread_array[MAX_THREADS+1];
  struct CommandInfo cmd_info_array[MAX_THREADS+1];

  while ((dp = readdir(dirp)) != NULL){
    if (!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..") || strstr(dp->d_name, ".out") )
      continue;
    
    dp_n++;
    int inputFd, outputFd, openFlags;
    pid_t pid;
    mode_t filePerms;
    char buffer[50];

    if (n_proc++ < MAX_PROC)
      pid = fork();

    if (pid < 0){
      fprintf(stderr, "Error creating process.\n");
      return -1;
    }
    if (!pid){
      strcpy(buffer, dirpath);
      strcat(buffer, dp->d_name);
      buffer[strlen(buffer)] = '\0';
      
      if((inputFd = open(buffer, O_RDONLY)) == -1){
        fprintf(stderr, "open error: %s\n", strerror(errno));
        return -1;
      };

      openFlags = O_CREAT | O_WRONLY | O_APPEND |O_TRUNC;
      filePerms = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH; 
      strtok(buffer, ".");
      strcat(buffer, ".out");

      if((outputFd = open(buffer, openFlags, filePerms)) == -1){
        fprintf(stderr, "open error: %s\n", strerror(errno));
        return -1;
      };

      for (int i = 1; i <= MAX_THREADS; i++){
        cmd_info_array[i].inputFd = inputFd;
        cmd_info_array[i].outputFd = outputFd;
        cmd_info_array[i].thread_id = (unsigned int)i;
        if (pthread_create(&thread_array[i], NULL, process_command, (void *)&cmd_info_array[i]) != 0) {
          fprintf(stderr, "Error creating thread\n");
          return -1;
        }
      }

      int result = 1;
      int result_thread;
      while(result){
        result = 0;
        for (int i = 1; i <= MAX_THREADS; i++){
          pthread_join(thread_array[i], (void**)&result_thread);
          if (result_thread == 1)
            result = 1;
        }
        barrier = 0;
        for (int i = 1; i <= MAX_THREADS; i++){
          cmd_info_array[i].inputFd = inputFd;
          cmd_info_array[i].outputFd = outputFd;
          cmd_info_array[i].thread_id = (unsigned int)i;
          if (pthread_create(&thread_array[i], NULL, process_command, (void *)&cmd_info_array[i]) != 0) {
            fprintf(stderr, "Error creating thread\n");
            return -1;
          }
        }
      }

      for (int i = 1; i <= MAX_THREADS; i++){
        pthread_join(thread_array[i], (void**)&result_thread);
        if (result_thread == 1)
          result = 1;
      }
    

      if(close (inputFd) == -1){
        fprintf(stderr, "Error closing file\n");
        return -1;
      }
      if(close (outputFd) == -1){
        fprintf(stderr, "Error closing file\n");
        return -1;
      }
      n_proc--;
      exit(0);
    }
  }
  int stat;
  for(int i = 0; i < dp_n; i++){
    pid_t cpid = wait(&stat);
    printf("Child %d terminated with status: %d\n", cpid, stat);
  }
  if(closedir(dirp) == -1){
    fprintf(stderr, "Error closing directory\n");
    return -1;
  }
  ems_terminate();
  return 0;
}
