#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include<sys/wait.h>

#include "constants.h"
#include "operations.h"
#include "parser.h"

int main(int argc, char *argv[]) {
  unsigned int state_access_delay_ms = STATE_ACCESS_DELAY_MS;
  int MAX_PROC, n_proc;

  n_proc = 0;
  MAX_PROC = atoi(argv[2]);

  if (argc > 3) {
    char *endptr;
    unsigned long int delay = strtoul(argv[3], &endptr, 10);

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

  while ((dp = readdir(dirp)) != NULL){
    if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0){
      continue;
    }
    int inputFd, outputFd, openFlags;
    mode_t filePerms;
    pid_t pid;
    char buffer[100];

    if (n_proc++ < MAX_PROC)
      pid = fork();

    if (pid < 0){
      fprintf(stderr, "Error creating process.\n");
      return -1;
    }
    if (pid == 0){
      strcpy(buffer, dirpath);
      strcat(buffer, dp->d_name);
      
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
        unsigned int event_id, delay;
        size_t num_rows, num_columns, num_coords;
        size_t xs[MAX_RESERVATION_SIZE], ys[MAX_RESERVATION_SIZE];

        switch (get_next(inputFd)) {
          case CMD_CREATE:
            if (parse_create(inputFd, &event_id, &num_rows, &num_columns) != 0) {
              fprintf(stderr, "Invalid command. See HELP for usage\n");
              continue;
            }

            if (ems_create(event_id, num_rows, num_columns)) {
              fprintf(stderr, "Failed to create event\n");
            }

            break;

          case CMD_RESERVE:
            num_coords = parse_reserve(inputFd, MAX_RESERVATION_SIZE, &event_id, xs, ys);

            if (num_coords == 0) {
              fprintf(stderr, "Invalid command. See HELP for usage\n");
              continue;
            }

            if (ems_reserve(event_id, num_coords, xs, ys)) {
              fprintf(stderr, "Failed to reserve seats\n");
            }

            break;

          case CMD_SHOW:
            if (parse_show(inputFd, &event_id) != 0) {
              fprintf(stderr, "Invalid command. See HELP for usage\n");
              continue;
            }

            if (ems_show(event_id, outputFd)) {
              fprintf(stderr, "Failed to show event\n");
            }

            break;

          case CMD_LIST_EVENTS:
            if (ems_list_events()) {
              fprintf(stderr, "Failed to list events\n");
            }

            break;

          case CMD_WAIT:
            if (parse_wait(inputFd, &delay, NULL) == -1) {  // thread_id is not implemented
              fprintf(stderr, "Invalid command. See HELP for usage\n");
              continue;
            }

            if (delay > 0) {
              printf("Waiting...\n");
              ems_wait(delay);
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
            endoffile = 1;
            break;
        }
      }
      if(close (inputFd) == -1){
        fprintf(stderr, "Error closing file\n");
        return -1;
      }
      n_proc--;
      exit(0);
    }
    else{
      wait(NULL);
    }
  }
  closedir(dirp);
  ems_terminate();
  return 0;
}
