#include "eventlist.h"

#include <stdlib.h>
#include <pthread.h>

pthread_mutex_t event_list_lock = PTHREAD_MUTEX_INITIALIZER;

struct EventList* create_list() {
  struct EventList* list = (struct EventList*)malloc(sizeof(struct EventList));
  if (!list) return NULL;
  pthread_mutex_lock(&event_list_lock);
  list->head = NULL;
  list->tail = NULL;
  pthread_mutex_unlock(&event_list_lock);
  return list;
}

int append_to_list(struct EventList* list, struct Event* event) {
  if (!list) return 1;

  struct ListNode* new_node = (struct ListNode*)malloc(sizeof(struct ListNode));
  if (!new_node) return 1;

  new_node->event = event;
  new_node->next = NULL;

  pthread_mutex_lock(&event_list_lock);
  if (list->head == NULL) {
    list->head = new_node;
    list->tail = new_node;
  } else {
    list->tail->next = new_node;
    list->tail = new_node;
  }
  pthread_mutex_unlock(&event_list_lock);

  return 0;
}

static void free_event(struct Event* event) {
  if (!event) return;

  free(event->data);
  free(event);
}

void free_list(struct EventList* list) {
  if (!list) return;

  struct ListNode* current = list->head;
  while (current) {
    struct ListNode* temp = current;
    current = current->next;

    free_event(temp->event);
    free(temp);
  }

  free(list);
}

struct Event* get_event(struct EventList* list, unsigned int event_id) {
  if (!list) return NULL;

  struct ListNode* current = list->head;
  while (current) {
    struct Event* event = current->event;
    if (event->id == event_id) {
      return event;
    }
    current = current->next;
  }

  return NULL;
}
