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
static event_params_t *retainedEvents[EVENT_BUS_BITS] = {0};

typedef struct {
  TaskHandle_t xCallingTask;
  uint32_t command;
  union {
    uint32_t *arrayParams;
    uint32_t params;
  };
  void *eventData;
} EVENT_CMD;

/* FreeRTOS Stack allocation */
#define STACK_SIZE configMINIMAL_STACK_SIZE
static StackType_t xStack[STACK_SIZE];
static StaticTask_t xTaskBuffer;
static StaticQueue_t xStaticQueue;
static uint8_t ucQueueStorage[EVENT_BUS_MAX_CMD_QUEUE * sizeof(EVENT_CMD)];
static QueueHandle_t xQueueCmd = NULL;

enum {
  CMD_ATTACH,
  CMD_DETACH,
  CMD_NEW_EVENT,
  CMD_INVALIDATE_EVENT,
  CMD_SUBSCRIBE_ADD,
  CMD_SUBSCRIBE_ADD_ARRAY,
  CMD_SUBSCRIBE_REMOVE
};

static void prvPublishEvent(event_params_t *eventParams, bool retain) {
  configASSERT(eventParams);
  configASSERT(eventParams->event < EVENT_BUS_BITS);

  if (retain) {
    retainedEvents[eventParams->event] = eventParams;
  } else {
    retainedEvents[eventParams->event] = NULL;
  }
  event_t *ev = firstEvent;
  while (ev != NULL) {
    if ((ev->eventMask[eventParams->event / 32] &
         (1UL << (eventParams->event % 32))) &&
        ev->callback != NULL) {
      ev->callback(eventParams);
    }
    ev = ev->next;
  }
}

static void prvSubscribeAdd(event_t *event, uint32_t newEvent) {
  configASSERT(newEvent < EVENT_BUS_BITS);
  event->eventMask[newEvent / 32] |= (1UL << (newEvent % 32));
  /* Search for any retained events */
  if (retainedEvents[newEvent] && event->callback != NULL) {
    event->callback(retainedEvents[newEvent]);
  }
}

static void prvSubscribeAddArray(event_t *event, uint32_t *eventList) {
  while (*eventList != EVENT_BUS_LAST_PARAM) {
    prvSubscribeAdd(event, *eventList);
    eventList++;
  }
}

static void prvSubscribeRemove(event_t *event, uint32_t remEvent) {
  event->eventMask[remEvent / 32] &= ~(1UL << (remEvent % 32));
}

static void prvAttachToBus(event_t *event) {
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
}

static void prvDetachFromBus(event_t *event) {
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

static void prvInvdaliteEvent(event_params_t *event) {
  configASSERT(event);
  configASSERT(event->event < EVENT_BUS_BITS);
  /* Delete previously retained event */
  retainedEvents[event->event] = NULL;
}

static void eventBusTasks(void *pvParameters) {
  static EVENT_CMD cmd;
  (void)pvParameters;
  for (;;) {
    xQueueReceive(xQueueCmd, &cmd, portMAX_DELAY);
    switch (cmd.command) {
    case CMD_ATTACH:
      prvAttachToBus(cmd.eventData);
      break;
    case CMD_DETACH:
      prvDetachFromBus(cmd.eventData);
      break;
    case CMD_NEW_EVENT:
      prvPublishEvent(cmd.eventData, cmd.params);
      break;
    case CMD_INVALIDATE_EVENT:
      prvInvdaliteEvent(cmd.eventData);
      break;
    case CMD_SUBSCRIBE_ADD:
      prvSubscribeAdd(cmd.eventData, cmd.params);
      break;
    case CMD_SUBSCRIBE_ADD_ARRAY:
      prvSubscribeAddArray(cmd.eventData, cmd.arrayParams);
      break;
    case CMD_SUBSCRIBE_REMOVE:
      prvSubscribeRemove(cmd.eventData, cmd.params);      
      break;
    default:
      break;
    }
    if (cmd.xCallingTask != NULL) {
      xTaskNotifyGive(cmd.xCallingTask);
    }
  }
}

void subEvent(event_t *event, uint32_t eventId) {
  configASSERT(event);
  configASSERT(eventId < EVENT_BUS_BITS);
  EVENT_CMD cmd = {
      .command = CMD_SUBSCRIBE_ADD, .eventData = event, .params = eventId};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  xQueueSendToBack(xQueueCmd, (void *)&cmd, portMAX_DELAY);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void subEventList(event_t *event, const uint32_t *eventList) {
  configASSERT(event);
  configASSERT(eventList);
  EVENT_CMD cmd = {.command = CMD_SUBSCRIBE_ADD_ARRAY,
                   .eventData = event,
                   .arrayParams = eventList};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  xQueueSendToBack(xQueueCmd, (void *)&cmd, portMAX_DELAY);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void unSubEvent(event_t *event, uint32_t eventId) {
  configASSERT(event);
  configASSERT(eventId < EVENT_BUS_BITS);
  EVENT_CMD cmd = {
      .command = CMD_SUBSCRIBE_REMOVE, .eventData = event, .params = eventId};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  xQueueSendToBack(xQueueCmd, (void *)&cmd, portMAX_DELAY);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void attachBus(event_t *event) {
  configASSERT(event);
  EVENT_CMD cmd = {.command = CMD_ATTACH, .eventData = event};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  xQueueSendToBack(xQueueCmd, (void *)&cmd, portMAX_DELAY);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void detachBus(event_t *event) {
  configASSERT(event);
  EVENT_CMD cmd = {.command = CMD_DETACH, .eventData = event};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  xQueueSendToBack(xQueueCmd, (void *)&cmd, portMAX_DELAY);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void publishEvent(event_params_t *event, bool retain) {
  configASSERT(event);
  EVENT_CMD cmd = {
      .command = CMD_NEW_EVENT, .eventData = event, .params = retain};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  xQueueSendToBack(xQueueCmd, (void *)&cmd, portMAX_DELAY);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

BaseType_t publishEventFromISR(event_params_t *event) {
  configASSERT(event);
  EVENT_CMD cmd = {
      .command = CMD_NEW_EVENT, .eventData = event, .params = 0};
  cmd.xCallingTask = NULL;
  return xQueueSendToBackFromISR(xQueueCmd, (void *)&cmd, NULL) == pdTRUE;
}

void publishEventQ(uint32_t event, uint32_t value) {
  event_params_t newEvent = {0};
  newEvent.event = event;
  newEvent.value = value;
  EVENT_CMD cmd = {
      .command = CMD_NEW_EVENT, .eventData = &newEvent, .params = 0};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  xQueueSendToBack(xQueueCmd, (void *)&cmd, portMAX_DELAY);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void invalidateEvent(event_params_t *event) {
  configASSERT(event);
  EVENT_CMD cmd = {.command = CMD_INVALIDATE_EVENT, .eventData = event};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  xQueueSendToBack(xQueueCmd, (void *)&cmd, portMAX_DELAY);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

TaskHandle_t initEventBus(void) {
  static TaskHandle_t processHandle = NULL;
  processHandle =
      xTaskCreateStatic(eventBusTasks, "Event-Bus", STACK_SIZE, NULL,
                        (configMAX_PRIORITIES - 2), xStack, &xTaskBuffer);
  xQueueCmd = xQueueCreateStatic(EVENT_BUS_MAX_CMD_QUEUE, sizeof(EVENT_CMD),
                                 ucQueueStorage, &xStaticQueue);
  return processHandle;
}