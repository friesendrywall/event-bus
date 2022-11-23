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
#include "mem_pool.h"

static event_listener_t *firstEvent = NULL;
static event_msg_t *retainedEvents[EVENT_BUS_BITS] = {0};

typedef enum {
  CMD_ATTACH,
  CMD_DETACH,
  CMD_NEW_EVENT,
  CMD_INVALIDATE_EVENT,
  CMD_SUBSCRIBE_ADD,
  CMD_SUBSCRIBE_ADD_ARRAY,
  CMD_SUBSCRIBE_REMOVE
} EVBUS_CMD_T;

typedef struct {
  EVBUS_CMD_T command;
  TaskHandle_t xCallingTask;
  union {
    const uint32_t *arrayParams;
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

/* Event memory pool */
#define POOL_SIZE_CALC(size) (size + sizeof(event_msg_t))
static uint8_t smEventPool[EVENT_BUS_POOL_SM_LN *
                           POOL_SIZE_CALC(EVENT_BUS_POOL_SM_SZ)] = {0};
static uint8_t lgEventPool[EVENT_BUS_POOL_LG_LN *
                           POOL_SIZE_CALC(EVENT_BUS_POOL_LG_SZ)] = {0};
static mp_pool_t mpSmall = {0};
static mp_pool_t mpLarge = {0};

static inline void prvSendEvent(event_listener_t *listener,
                                event_msg_t *eventParams) {
  if (listener->callback != NULL) {
    listener->callback(eventParams);
  } else if (listener->queueHandle != NULL) {
    if (xQueueSendToBackFromISR(listener->queueHandle, (void *)&eventParams, NULL) != pdTRUE) {
      listener->errFull = 1;
      EVENT_BUS_DEBUG_QUEUE_FULL(listener->name);
    } else if (eventParams->dynamicAlloc) {
      eventParams->refCount++;
    }
  } else if (listener->waitingTask != NULL) {
    xTaskNotifyGive(listener->waitingTask);
  }
}

static void prvPublishEvent(event_msg_t *eventParams, bool retain) {
  configASSERT(eventParams);
  configASSERT(eventParams->event < EVENT_BUS_BITS);

  if (retain) {
    retainedEvents[eventParams->event] = eventParams;
  } else {
    retainedEvents[eventParams->event] = NULL;
  }
  event_listener_t *ev = firstEvent;
  while (ev != NULL) {
    if ((ev->eventMask[eventParams->event / 32] &
         (1UL << (eventParams->event % 32)))) {
      prvSendEvent(ev, eventParams);
    }
    ev = ev->next;
  }
  /* If no subscribers, make sure event is free'd */
  if (eventParams->dynamicAlloc && eventParams->refCount == 0) {
    vTaskSuspendAll();
    if (eventParams->lg) {
      mp_free(&mpLarge, eventParams);
    } else {
      mp_free(&mpSmall, eventParams);
    }
    xTaskResumeAll();
  }
}

static void prvSubscribeAdd(event_listener_t *listener, uint32_t newEvent) {
  configASSERT(newEvent < EVENT_BUS_BITS);
  listener->eventMask[newEvent / 32] |= (1UL << (newEvent % 32));
  /* Search for any retained events */
  if (retainedEvents[newEvent]) {
    prvSendEvent(listener, retainedEvents[newEvent]);
  }
}

static void prvSubscribeAddArray(event_listener_t *listener,
                                 const uint32_t *eventList) {
  while (*eventList != EVENT_BUS_LAST_PARAM) {
    prvSubscribeAdd(listener, *eventList);
    eventList++;
  }
}

static void prvSubscribeRemove(event_listener_t *listener, uint32_t remEvent) {
  listener->eventMask[remEvent / 32] &= ~(1UL << (remEvent % 32));
}

static void prvAttachToBus(event_listener_t *listener) {
  event_listener_t *ev;
  configASSERT(listener);
  if (firstEvent == NULL) {
    firstEvent = listener;
    firstEvent->prev = NULL;
    firstEvent->next = NULL;
  } else {
    // Walk the list
    ev = firstEvent;
    for (;;) {
      if (ev->next == NULL) {
        ev->next = listener;
        listener->prev = ev;
        listener->next = NULL;
        break;
      } else {
        ev = ev->next;
      }
    }
  }
}

static void prvDetachFromBus(event_listener_t *listener) {
  configASSERT(listener);
  /* If first one */
  if (listener->prev == NULL) {
    /* If none following */
    if (listener->next == NULL) {
      firstEvent = NULL;
    } else {
      firstEvent = listener->next;
      firstEvent->prev = NULL;
    }
  } else {
    if (listener->next != NULL) {
      listener->prev->next = listener->next;
      listener->next->prev = listener->prev;
    } else {
      listener->prev->next = NULL;
    }
  }
  listener->next = listener->prev = NULL;
}

static void prvInvdaliteEvent(event_msg_t *event) {
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
#ifdef EVENT_BUS_USE_TASK_NOTIFICATION_INDEX
    if (cmd.xCallingTask != NULL) {
      xTaskNotifyGiveIndexed(cmd.xCallingTask,
                             EVENT_BUS_USE_TASK_NOTIFICATION_INDEX);
    }
#endif
  }
}

void subEvent(event_listener_t *listener, uint32_t eventId) {
  configASSERT(listener);
  configASSERT(eventId < EVENT_BUS_BITS);
  EVENT_CMD cmd = {
      .command = CMD_SUBSCRIBE_ADD, .eventData = listener, .params = eventId};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  xQueueSendToBack(xQueueCmd, (void *)&cmd, portMAX_DELAY);
#ifdef EVENT_BUS_USE_TASK_NOTIFICATION_INDEX
  ulTaskNotifyTakeIndexed(EVENT_BUS_USE_TASK_NOTIFICATION_INDEX, pdTRUE,
                          portMAX_DELAY);
#else
  taskYIELD();
#endif
}

void subEventList(event_listener_t *listener, const uint32_t *eventList) {
  configASSERT(listener);
  configASSERT(eventList);
  EVENT_CMD cmd = {.command = CMD_SUBSCRIBE_ADD_ARRAY,
                   .eventData = listener,
                   .arrayParams = eventList};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  xQueueSendToBack(xQueueCmd, (void *)&cmd, portMAX_DELAY);
#ifdef EVENT_BUS_USE_TASK_NOTIFICATION_INDEX
  ulTaskNotifyTakeIndexed(EVENT_BUS_USE_TASK_NOTIFICATION_INDEX, pdTRUE,
                          portMAX_DELAY);
#else
  taskYIELD();
#endif
}

void unSubEvent(event_listener_t *listener, uint32_t eventId) {
  configASSERT(listener);
  configASSERT(eventId < EVENT_BUS_BITS);
  EVENT_CMD cmd = {.command = CMD_SUBSCRIBE_REMOVE,
                   .eventData = listener,
                   .params = eventId};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  xQueueSendToBack(xQueueCmd, (void *)&cmd, portMAX_DELAY);
#ifdef EVENT_BUS_USE_TASK_NOTIFICATION_INDEX
  ulTaskNotifyTakeIndexed(EVENT_BUS_USE_TASK_NOTIFICATION_INDEX, pdTRUE,
                          portMAX_DELAY);
#else
  taskYIELD();
#endif
}

void attachBus(event_listener_t *listener) {
  configASSERT(listener);
  EVENT_CMD cmd = {.command = CMD_ATTACH, .eventData = listener};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  /* Subscribing task must have lower priority or weird things will happen */
  configASSERT(uxTaskPriorityGet(NULL) < EVENT_BUS_RTOS_PRIORITY);
  xQueueSendToBack(xQueueCmd, (void *)&cmd, portMAX_DELAY);
#ifdef EVENT_BUS_USE_TASK_NOTIFICATION_INDEX
  ulTaskNotifyTakeIndexed(EVENT_BUS_USE_TASK_NOTIFICATION_INDEX, pdTRUE,
                          portMAX_DELAY);
#else
  taskYIELD();
#endif
}

void detachBus(event_listener_t *listener) {
  configASSERT(listener);
  EVENT_CMD cmd = {.command = CMD_DETACH, .eventData = listener};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  xQueueSendToBack(xQueueCmd, (void *)&cmd, portMAX_DELAY);
#ifdef EVENT_BUS_USE_TASK_NOTIFICATION_INDEX
  ulTaskNotifyTakeIndexed(EVENT_BUS_USE_TASK_NOTIFICATION_INDEX, pdTRUE,
                          portMAX_DELAY);
#else
  taskYIELD();
#endif
}

void publishEvent(void *event, bool retain) {
  configASSERT(event);
  configASSERT(((event_msg_t *)event)->event < EVENT_BUS_BITS);
  EVENT_CMD cmd = {
      .command = CMD_NEW_EVENT, .eventData = event, .params = retain};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  xQueueSendToBack(xQueueCmd, (void *)&cmd, portMAX_DELAY);
#ifdef EVENT_BUS_USE_TASK_NOTIFICATION_INDEX
  ulTaskNotifyTakeIndexed(EVENT_BUS_USE_TASK_NOTIFICATION_INDEX, pdTRUE,
                          portMAX_DELAY);
#else
  taskYIELD();
#endif
}

BaseType_t publishEventFromISR(void *event) {
  configASSERT(event);
  configASSERT(((event_msg_t *)event)->event < EVENT_BUS_BITS);
  EVENT_CMD cmd = {
      .command = CMD_NEW_EVENT, .eventData = event, .params = 0};
  cmd.xCallingTask = NULL;
  return xQueueSendToBackFromISR(xQueueCmd, (void *)&cmd, NULL) == pdTRUE;
}

void invalidateEvent(void *event) {
  configASSERT(event);
  EVENT_CMD cmd = {.command = CMD_INVALIDATE_EVENT, .eventData = event};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  xQueueSendToBack(xQueueCmd, (void *)&cmd, portMAX_DELAY);
#ifdef EVENT_BUS_USE_TASK_NOTIFICATION_INDEX
  ulTaskNotifyTakeIndexed(EVENT_BUS_USE_TASK_NOTIFICATION_INDEX, pdTRUE,
                          portMAX_DELAY);
#else
  taskYIELD();
#endif
}

#ifdef EVENT_BUS_USE_TASK_NOTIFICATION_INDEX
BaseType_t waitEvent(uint32_t event, uint32_t waitTicks) {
  event_listener_t listener = {0};
  listener.waitingTask = xTaskGetCurrentTaskHandle();
  attachBus(&listener);
  subEvent(&listener, event);
  uint32_t ret = ulTaskNotifyTake(pdTRUE, waitTicks);
  detachBus(&listener);
  if (ret == 0) {
    /* Make sure we didn't get one from a race cond */
    ret = ulTaskNotifyTake(pdTRUE, 0);
  }
  return ret == 1 ? pdPASS : pdFAIL;
}
#endif

TaskHandle_t initEventBus(void) {
  static TaskHandle_t processHandle = NULL;
#ifdef EVENT_BUS_USE_TASK_NOTIFICATION_INDEX
  configASSERT(EVENT_BUS_USE_TASK_NOTIFICATION_INDEX > 0);
#endif
  processHandle =
      xTaskCreateStatic(eventBusTasks, "Event-Bus", STACK_SIZE, NULL,
                        EVENT_BUS_RTOS_PRIORITY, xStack, &xTaskBuffer);
  xQueueCmd = xQueueCreateStatic(EVENT_BUS_MAX_CMD_QUEUE, sizeof(EVENT_CMD),
                                 ucQueueStorage, &xStaticQueue);

  mp_init(EVENT_BUS_POOL_SM_LN, POOL_SIZE_CALC(EVENT_BUS_POOL_SM_SZ),
          smEventPool, &mpSmall);
  mp_init(EVENT_BUS_POOL_LG_LN, POOL_SIZE_CALC(EVENT_BUS_POOL_LG_SZ),
          lgEventPool, &mpLarge);
  return processHandle;
}

static void *prvEventAlloc(size_t size, uint32_t eventId, uint16_t publisherId,
                           uint16_t refCount) {
  event_msg_t *val;
  configASSERT(size >= sizeof(event_msg_t));
  configASSERT(size <= POOL_SIZE_CALC(EVENT_BUS_POOL_LG_SZ));
  vTaskSuspendAll();
  if (size > POOL_SIZE_CALC(EVENT_BUS_POOL_SM_SZ)) {
    val = mp_malloc(&mpLarge);
  } else {
    val = mp_malloc(&mpSmall);
  }
  if (val != NULL) {
    val->lg = size > POOL_SIZE_CALC(EVENT_BUS_POOL_SM_SZ);
    val->dynamicAlloc = 1;
    val->refCount = refCount;
    val->event = eventId;
    val->publisherId = publisherId;
  }
  xTaskResumeAll();
  return (void *)val;
}

void *eventAlloc(size_t size, uint32_t eventId, uint16_t publisherId) {
  /* Zero ref count */
  return prvEventAlloc(size, eventId, publisherId, 0);
}

void *threadEventAlloc(size_t size, uint32_t eventId, uint16_t publisherId) {
  /* Single ref count */
  return prvEventAlloc(size, eventId, publisherId, 1);
}

void eventRelease(void *event) {
  event_msg_t *e = event;
  configASSERT(e->dynamicAlloc);
  configASSERT(e->refCount > 0); /* Too many releases */
  vTaskSuspendAll();
  if (e->dynamicAlloc) {
    e->refCount--;
    if (e->refCount == 0) {
      if (e->lg) {
        mp_free(&mpLarge, event);
      } else {
        mp_free(&mpSmall, event);
      }
    }
  }
  xTaskResumeAll();
}