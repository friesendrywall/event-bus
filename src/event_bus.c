/*
 * File:   eventBus.c
 * Author: Erik Friesen
 *
 * Created on September 14, 2022, 9:27 AM
 */

#include <stdint.h>
#include <stdio.h>
/* Kernel includes. */
#include "event_bus.h"
#include <FreeRTOS.h>
#include <queue.h>
#include <semphr.h>
#include <task.h>
#include <timers.h>

static event_t *firstEvent = NULL;
static event_params_t *retainedEvents[EVENT_BUS_BITS];

typedef struct {
  TaskHandle_t xCallingTask;
  uint32_t command;
  void *eventData;
} EVENT_CMD;

/* FreeRTOS Stack allocation */
#define STACK_SIZE configMINIMAL_STACK_SIZE
#define MAX_CMD_QUEUE 16
static StackType_t xStack[STACK_SIZE];
static StaticTask_t xTaskBuffer;
static StaticQueue_t xStaticQueue;
static uint8_t ucQueueStorage[MAX_CMD_QUEUE * sizeof(EVENT_CMD)];
static TaskHandle_t processHandle = NULL;
static QueueHandle_t xQueueCmd = NULL;

enum { CMD_SUBSCRIBE, CMD_UNSUBSCRIBE, CMD_NEW_EVENT, CMD_INVALIDATE_EVENT };

static uint32_t multipleBitsSet(uint32_t n) { return n & (n - 1); }

static void prvPublishEvent(event_params_t *eventParams) {
  configASSERT(eventParams);
  configASSERT(!multipleBitsSet(eventParams->event));
  configASSERT(eventParams->event);
  uint32_t index = 0;
  uint32_t e = eventParams->event;
  while (e >>= 1) {
    index++;
  }
  if (eventParams->flags & EVENT_BUS_FLAGS_RETAIN) {
    retainedEvents[index] = eventParams;
  } else {
    retainedEvents[index] = NULL;
  }
  event_t *ev = firstEvent;
  while (ev != NULL) {
    if ((ev->eventMask & eventParams->event) && ev->callback != NULL) {
      ev->callback(eventParams);
    }
    ev = ev->next;
  }
}

static void prvSubscribeEvent(event_t *event) {
  uint32_t i;
  configASSERT(event);
  event_t *ev;
  if (firstEvent == NULL) {
    firstEvent = event;
    firstEvent->prev = NULL;
    firstEvent->next = NULL;
  } else {
    // Walk the list
    ev = firstEvent;
    for (;;) {
      if (ev->next == NULL) {
        ev->next = event;
        event->prev = ev;
        event->next = NULL;
        break;
      } else {
        ev = ev->next;
      }
    }
  }
  /* Search for any retained events */
  for (i = 0; i < EVENT_BUS_BITS; i++) {
    if (((1UL << i) & event->eventMask) && retainedEvents[i] &&
        (retainedEvents[i]->flags & EVENT_BUS_FLAGS_RETAIN) &&
        event->callback != NULL) {
      event->callback(retainedEvents[i]);
      break;
    }
  }
}

static void prvUnSubscribeEvent(event_t *event) {
  configASSERT(event);
  /* If first one */
  if (event->prev == NULL) {
    /* If none following */
    if (event->next == NULL) {
      firstEvent = NULL;
    } else {
      firstEvent = event->next;
      firstEvent->prev = NULL;
    }
  } else {
    if (event->next != NULL) {
      event->prev->next = event->next;
      event->next->prev = event->prev;
    } else {
      event->prev->next = NULL;
    }
  }
  event->next = event->prev = NULL;
}

static void prvInvdaliteEvent(event_t *event) {
  uint32_t i;
  configASSERT(event);
  /* Delete previously retained event */
  for (i = 0; i < EVENT_BUS_BITS; i++) {
    if (((1UL << i) & event->eventMask) && retainedEvents[i] &&
        (retainedEvents[i]->flags & EVENT_BUS_FLAGS_RETAIN)) {
      retainedEvents[i] = NULL;
      break;
    }
  }
}

static void eventBusTasks(void *pvParameters) {
  static EVENT_CMD cmd;
  event_t *ev; 
  (void)pvParameters;
  for (;;) {
    xQueueReceive(xQueueCmd, &cmd, portMAX_DELAY);
    switch (cmd.command) {
    case CMD_SUBSCRIBE:
      prvSubscribeEvent(cmd.eventData);
      xTaskNotifyGive(cmd.xCallingTask);
      break;
    case CMD_UNSUBSCRIBE:
      prvUnSubscribeEvent(cmd.eventData);
      xTaskNotifyGive(cmd.xCallingTask);
      break;
    case CMD_NEW_EVENT:
      prvPublishEvent(cmd.eventData);
      xTaskNotifyGive(cmd.xCallingTask);
      break;
    case CMD_INVALIDATE_EVENT:
      prvInvdaliteEvent(cmd.eventData);
      xTaskNotifyGive(cmd.xCallingTask);
      break;
    default:
      break;
    }
  }
}

void subscribeEvent(event_t *event) {
  configASSERT(event);
  EVENT_CMD cmd;
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  cmd.command = CMD_SUBSCRIBE;
  cmd.eventData = event;
  xQueueSendToFront(xQueueCmd, (void *)&cmd, portMAX_DELAY);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void unSubscribeEvent(event_t *event) {
  configASSERT(event);
  EVENT_CMD cmd;
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  cmd.command = CMD_UNSUBSCRIBE;
  cmd.eventData = event;
  xQueueSendToFront(xQueueCmd, (void *)&cmd, portMAX_DELAY);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void publishEvent(event_params_t *event) {
  configASSERT(event);
  EVENT_CMD cmd;
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  cmd.command = CMD_NEW_EVENT;
  cmd.eventData = event;
  xQueueSendToFront(xQueueCmd, (void *)&cmd, portMAX_DELAY);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void invalidateEvent(event_params_t *event) {
  configASSERT(event);
  EVENT_CMD cmd;
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  cmd.command = CMD_INVALIDATE_EVENT;
  cmd.eventData = event;
  xQueueSendToFront(xQueueCmd, (void *)&cmd, portMAX_DELAY);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void initEventBus(void) {
  processHandle = xTaskCreateStatic(eventBusTasks, "Event-Bus", 
      STACK_SIZE, NULL, (configMAX_PRIORITIES - 2), xStack, &xTaskBuffer);
  xQueueCmd = xQueueCreateStatic(MAX_CMD_QUEUE, sizeof(EVENT_CMD),
                                 ucQueueStorage, &xStaticQueue);
  configASSERT(xQueueCmd != NULL);
}