/**
 * MIT License
 *
 * Copyright (c) 2022 Erik Friesen
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdint.h>
#include <stdio.h>
/* Kernel includes. */
#include <FreeRTOS.h>
#include <queue.h>
#include <semphr.h>
#include <task.h>
#include <timers.h>
#include "event_bus.h"
#include "event_bus_config.h"

static event_t *firstEvent = NULL;
static event_params_t *retainedEvents[EVENT_BUS_BITS];

typedef struct {
  TaskHandle_t xCallingTask;
  uint32_t command;
  uint32_t params;
  uint32_t callerId;
  void *eventData;
} EVENT_CMD;

/* FreeRTOS Stack allocation */
#define STACK_SIZE configMINIMAL_STACK_SIZE
static StackType_t xStack[STACK_SIZE];
static StaticTask_t xTaskBuffer;
static StaticQueue_t xStaticQueue;
static uint8_t ucQueueStorage[EVENT_BUS_MAX_CMD_QUEUE * sizeof(EVENT_CMD)];
static TaskHandle_t processHandle = NULL;
static QueueHandle_t xQueueCmd = NULL;

enum {
  CMD_SUBSCRIBE,
  CMD_UNSUBSCRIBE,
  CMD_NEW_EVENT,
  CMD_INVALIDATE_EVENT,
  CMD_SUBSCRIBE_ADD,
  CMD_SUBSCRIBE_REMOVE
};

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

static void prvSubscribeAdd(event_t *event, uint32_t newEventMask) {
  uint32_t i;
  event->eventMask |= newEventMask;
  /* Search for any retained events */
  for (i = 0; i < EVENT_BUS_BITS; i++) {
    if (((1UL << i) & newEventMask) && retainedEvents[i] &&
        (retainedEvents[i]->flags & EVENT_BUS_FLAGS_RETAIN) &&
        event->callback != NULL) {
      event->callback(retainedEvents[i]);
      break;
    }
  }
}

static void prvSubscribeRemove(event_t *event, uint32_t newEventMask) {
  event->eventMask &= ~(newEventMask);
}

static void prvSubscribeEvent(event_t *event) {
  uint32_t i;
  event_t *ev;
  configASSERT(event);
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
    case CMD_SUBSCRIBE_ADD:
      prvSubscribeAdd(cmd.eventData, cmd.params);
      xTaskNotifyGive(cmd.xCallingTask);
      break;
    case CMD_SUBSCRIBE_REMOVE:
      prvSubscribeRemove(cmd.eventData, cmd.params);
      xTaskNotifyGive(cmd.xCallingTask);
      break;
    default:
      break;
    }
  }
}

void subscribeAdd(event_t *event, uint32_t eventMask) {
  configASSERT(event);
  EVENT_CMD cmd = {
      .command = CMD_SUBSCRIBE_ADD, .eventData = event, .params = eventMask};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  xQueueSendToFront(xQueueCmd, (void *)&cmd, portMAX_DELAY);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void subscribeRemove(event_t *event, uint32_t eventMask) {
  configASSERT(event);
  EVENT_CMD cmd = {
      .command = CMD_SUBSCRIBE_REMOVE, .eventData = event, .params = eventMask};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  xQueueSendToFront(xQueueCmd, (void *)&cmd, portMAX_DELAY);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void subscribeEvent(event_t *event) {
  configASSERT(event);
  EVENT_CMD cmd = {.command = CMD_SUBSCRIBE, .eventData = event};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  xQueueSendToFront(xQueueCmd, (void *)&cmd, portMAX_DELAY);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void unSubscribeEvent(event_t *event) {
  configASSERT(event);
  EVENT_CMD cmd = {.command = CMD_UNSUBSCRIBE, .eventData = event};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  xQueueSendToFront(xQueueCmd, (void *)&cmd, portMAX_DELAY);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void publishEvent(event_params_t *event) {
  configASSERT(event);
  EVENT_CMD cmd = {.command = CMD_NEW_EVENT, .eventData = event};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  xQueueSendToFront(xQueueCmd, (void *)&cmd, portMAX_DELAY);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void invalidateEvent(event_params_t *event) {
  configASSERT(event);
  EVENT_CMD cmd = {.command = CMD_INVALIDATE_EVENT, .eventData = event};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  xQueueSendToFront(xQueueCmd, (void *)&cmd, portMAX_DELAY);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void initEventBus(void) {
  processHandle = xTaskCreateStatic(eventBusTasks, "Event-Bus", 
      STACK_SIZE, NULL, (configMAX_PRIORITIES - 2), xStack, &xTaskBuffer);
  xQueueCmd = xQueueCreateStatic(EVENT_BUS_MAX_CMD_QUEUE, sizeof(EVENT_CMD),
                                 ucQueueStorage, &xStaticQueue);
  configASSERT(xQueueCmd != NULL);
}

TaskHandle_t eventBusProcessHandle(void) {
  return processHandle;
}