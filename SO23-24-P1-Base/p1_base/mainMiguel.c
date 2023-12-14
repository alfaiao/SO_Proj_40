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
    enum Command command;
    int inputFd;
    int outputFd;
    unsigned int event_id;
    size_t num_rows;
    size_t num_columns;
    size_t num_coords;
    size_t xs[MAX_RESERVATION_SIZE];
    size_t ys[MAX_RESERVATION_SIZE];
    unsigned int delay;
    int endoffile;
    pthread_mutex_t mutex;
};


void *process_command(void *arg) {
  struct CommandInfo *cmd_info = (struct CommandInfo *)arg;

  pthread_mutex_lock(&cmd_info->mutex);

  switch (cmd_info->command) {
        case CMD_CREATE:
            if (parse_create(cmd_info->inputFd, &cmd_info->event_id, &cmd_info->num_rows, &cmd_info->num_columns) != 0) {
                fprintf(stderr, "Invalid command. See HELP for usage\n");
                break;
            }

            if (ems_create(cmd_info->event_id, cmd_info->num_rows, cmd_info->num_columns)) {
                fprintf(stderr, "Failed to create event\n");
            }

            break;

        case CMD_RESERVE:
            cmd_info->num_coords = parse_reserve(cmd_info->inputFd, MAX_RESERVATION_SIZE, &cmd_info->event_id, cmd_info->xs, cmd_info->ys);

            if (cmd_info->num_coords == 0) {
                fprintf(stderr, "Invalid command. See HELP for usage\n");
                break;
            }

            if (ems_reserve(cmd_info->event_id, cmd_info->num_coords, cmd_info->xs, cmd_info->ys)) {
                fprintf(stderr, "Failed to reserve seats\n");
            }

            break;

        case CMD_SHOW:
            if (parse_show(cmd_info->inputFd, &cmd_info->event_id) != 0) {
                fprintf(stderr, "Invalid command. See HELP for usage\n");
            } else {
                if (ems_show(cmd_info->event_id, cmd_info->outputFd)) {
                    fprintf(stderr, "Failed to show event\n");
                }
            }
            break;

        case CMD_LIST_EVENTS:
            if (ems_list_events()) {
                fprintf(stderr, "Failed to list events\n");
            }
            break;

        case CMD_WAIT:
            if (parse_wait(cmd_info->inputFd, &cmd_info->delay, NULL) == -1) {
                fprintf(stderr, "Invalid command. See HELP for usage\n");
            } else {
                if (cmd_info->delay > 0) {
                    printf("Waiting...\n");
                    ems_wait(cmd_info->delay);
                }
            }
            break;

        case CMD_INVALID:
            fprintf(stderr, "Invalid command. See HELP for usage\n");
            break;

        case CMD_HELP:
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

        case CMD_BARRIER:  // Not implemented
        case CMD_EMPTY:
            break;

        case EOC:
            cmd_info->endoffile = 1;
            break;
  }
  pthread_mutex_unlock(&cmd_info->mutex);
  
  pthread_exit(NULL);
}




int main(int argc, char *argv[]) {
  unsigned int state_access_delay_ms = STATE_ACCESS_DELAY_MS;
  int MAX_PROC, n_proc, MAX_THREADS ;
  

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
  pthread_t thread_array[MAX_THREADS];  
  int thread_count = 0;
  struct CommandInfo cmd_info_array[MAX_THREADS];

  for (int i = 0; i < MAX_THREADS; ++i) {
      pthread_mutex_init(&cmd_info_array[i].mutex, NULL);
  }


  while ((dp = readdir(dirp)) != NULL){
    if (!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, ".."))
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

      openFlags = O_CREAT | O_WRONLY | O_APPEND;
      filePerms = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH; 
      strtok(buffer, ".");
      strcat(buffer, ".out");

      if((outputFd = open(buffer, openFlags, filePerms)) == -1){
        fprintf(stderr, "open error: %s\n", strerror(errno));
        return -1;
      };

      int endoffile = 0;
      while (!endoffile) {
        struct CommandInfo *cmd_info = &cmd_info_array[thread_count];
        cmd_info->inputFd = inputFd;
        cmd_info->outputFd = outputFd;
        cmd_info->endoffile = 0; 
        cmd_info->command = get_next(inputFd); 

        if (pthread_create(&thread_array[thread_count++], NULL, process_command, (void *)cmd_info) != 0) {
        fprintf(stderr, "Error creating thread\n");
        return -1;
        }

        if (thread_count == MAX_THREADS) {
          for (int i = 0; i < MAX_THREADS; ++i) {
            pthread_join(thread_array[i], NULL);
          }
          thread_count = 0;
        }

        if (cmd_info->command == EOC) {
        endoffile = 1;
        }
        
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

// Aguarda threads restantes antes de prosseguir 
  for (int i = 0; i < thread_count; ++i) {
        pthread_join(thread_array[i], NULL);
        pthread_mutex_destroy(&cmd_info_array[i].mutex);
    }

  ems_terminate();
  return 0;
}