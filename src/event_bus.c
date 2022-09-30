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
#include <string.h>
/* Kernel includes. */
#include <FreeRTOS.h>
#include <message_buffer.h>
#include <queue.h>
#include <semphr.h>
#include <task.h>
#include <timers.h>
#include "event_bus.h"

static event_t *firstEvent = NULL;
static event_msg_t retainedEvents[EVENT_BUS_BITS];

typedef struct {
  TaskHandle_t xCallingTask;
  uint32_t command;
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

enum { CMD_SUBSCRIBE, CMD_UNSUBSCRIBE, CMD_NEW_EVENT, CMD_INVALIDATE_EVENT };

static uint32_t multipleBitsSet(uint32_t n) { return n & (n - 1); }

static void prvPublishEvent(event_params_t *eventParams) {
  uint32_t index = 0;
  configASSERT(eventParams);
  configASSERT(!multipleBitsSet(eventParams->event));
  configASSERT(eventParams->event);
  configASSERT(eventParams->len < EVENT_BUS_MAX_DATA_LEN);

  uint32_t e = eventParams->event;
  /* Find index */
  while (e >>= 1) {
    index++;
  }
  /* Build message */
  event_msg_t *msg = &retainedEvents[index];
  msg->event = eventParams->event;
  msg->flags = 0;
  msg->len = eventParams->len;
  memcpy(msg->data, eventParams->ptr, eventParams->len);
  /* Walk the list */
  event_t *ev = firstEvent;
  while (ev != NULL) {
    if ((ev->eventMask & eventParams->event) && ev->msgBuffHandle != NULL &&
        ev->msgBuffHandle != eventParams->ignore) {
      size_t res = xMessageBufferSendFromISR(
          ev->msgBuffHandle, msg,
          offsetof(event_msg_t, data) + eventParams->len, NULL);
      if (res != offsetof(event_msg_t, data) + eventParams->len) {
        ev->errFull = 1;
      }
    }
    ev = ev->next;
  }
  /* Mark flags here so future requests know real status */
  msg->flags = eventParams->flags;
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
    event_msg_t *msg = (event_msg_t *)&retainedEvents[i];
    if (((1UL << i) & event->eventMask) && msg->event &&
        (msg->flags & EVENT_BUS_FLAGS_RETAIN) && event->msgBuffHandle != NULL) {
      size_t res = xMessageBufferSendFromISR(
          event->msgBuffHandle, msg, offsetof(event_msg_t, data) + msg->len,
          NULL);
      if (res != offsetof(event_msg_t, data) + msg->len) {
        event->errFull = 1;
      }
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
    event_msg_t *msg = (event_msg_t *)&retainedEvents[i];
    if (((1UL << i) & event->eventMask) && msg->event &&
        (msg->flags & EVENT_BUS_FLAGS_RETAIN)) {
      msg->flags = 0;
      msg->event = 0;
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
    default:
      break;
    }
  }
}

void subscribeEvent(event_t *event) {
  configASSERT(event);
  EVENT_CMD cmd = {.command = CMD_SUBSCRIBE};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  cmd.eventData = event;
  xQueueSendToBack(xQueueCmd, (void *)&cmd, portMAX_DELAY);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void unSubscribeEvent(event_t *event) {
  configASSERT(event);
  EVENT_CMD cmd = {.command = CMD_UNSUBSCRIBE};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  cmd.eventData = event;
  xQueueSendToBack(xQueueCmd, (void *)&cmd, portMAX_DELAY);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void publishEvent(event_params_t *event) {
  configASSERT(event);
  EVENT_CMD cmd = {.command = CMD_NEW_EVENT};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  cmd.eventData = event;
  xQueueSendToBack(xQueueCmd, (void *)&cmd, portMAX_DELAY);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void invalidateEvent(event_params_t *event) {
  configASSERT(event);
  EVENT_CMD cmd = {.command = CMD_INVALIDATE_EVENT};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  cmd.eventData = event;
  xQueueSendToBack(xQueueCmd, (void *)&cmd, portMAX_DELAY);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void initEventBus(void) {
  processHandle =
      xTaskCreateStatic(eventBusTasks, "Event-Bus", STACK_SIZE, NULL,
                        (configMAX_PRIORITIES - 2), xStack, &xTaskBuffer);
  xQueueCmd = xQueueCreateStatic(EVENT_BUS_MAX_CMD_QUEUE, sizeof(EVENT_CMD),
                                 ucQueueStorage, &xStaticQueue);
  configASSERT(xQueueCmd != NULL);
}

TaskHandle_t eventBusProcessHandle(void) {
  return processHandle;
}