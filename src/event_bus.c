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

static event_listener_t *firstListener = NULL;
static event_t *retainedEvents[EVENT_BUS_BITS] = {0};
static volatile uint32_t eventMaxResponse[EVENT_BUS_BITS];
static volatile uint32_t eventMinResponse[EVENT_BUS_BITS];

typedef enum {
  CMD_ATTACH,
  CMD_DETACH,
  CMD_NEW_EVENT,
  CMD_INVALIDATE_EVENT,
  CMD_SUBSCRIBE_ADD,
  CMD_SUBSCRIBE_ADD_ARRAY,
  CMD_SUBSCRIBE_REMOVE
} EVBUS_CMD_T;

typedef enum {
  DYN_ALLOC_NONE,
  DYN_ALLOC_SMALL,
  DYN_ALLOC_MED,
  DYN_ALLOC_LARGE
} DYN_ALLOC_T;

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
#if EVENT_BUS_DYNAMIC_FREERTOS != 1
static StackType_t xStack[STACK_SIZE];
static StaticTask_t xTaskBuffer;
static StaticQueue_t xStaticQueue;
static uint8_t ucQueueStorage[EVENT_BUS_MAX_CMD_QUEUE * sizeof(EVENT_CMD)];
#endif
static QueueHandle_t xQueueCmd = NULL;

/* Event memory pool */
#define POOL_SIZE_CALC(size) (size + sizeof(event_t))
static uint8_t smEventPool[EVENT_BUS_POOL_SM_CT *
                           POOL_SIZE_CALC(EVENT_BUS_POOL_SM_SZ)] = {0};
static uint8_t mdEventPool[EVENT_BUS_POOL_MD_CT *
                           POOL_SIZE_CALC(EVENT_BUS_POOL_MD_SZ)] = {0};
static uint8_t lgEventPool[EVENT_BUS_POOL_LG_CT *
                           POOL_SIZE_CALC(EVENT_BUS_POOL_LG_SZ)] = {0};
static mp_pool_t mpSmall = {0};
static mp_pool_t mpMed = {0};
static mp_pool_t mpLarge = {0};

static inline void prvSendEvent(event_listener_t *listener,
                                event_t *eventParams) {
  if (listener->callback != NULL) {
    listener->callback(eventParams);
  } else if (listener->queueHandle != NULL) {
    if (xQueueSendToBackFromISR(listener->queueHandle, (void *)&eventParams,
                                NULL) != pdTRUE) {
      listener->errFull = 1;
      EVENT_BUS_DEBUG_QUEUE_FULL(listener->name);
    } else if (eventParams->dynamicAlloc) {
      vTaskSuspendAll();
      eventParams->refCount++;
      listener->refCount++;
      xTaskResumeAll();
    }
  } else if (listener->waitingTask != NULL) {
    xTaskNotifyGive(listener->waitingTask);
  }
}

static void prvPublishEvent(event_t *eventParams, bool retain) {
  configASSERT(eventParams);
  configASSERT(eventParams->event < EVENT_BUS_BITS);
  eventParams->publishTime = EVENT_BUS_TIME_SOURCE;
  eventParams->published = 1;
  if (retain) {
    retainedEvents[eventParams->event] = eventParams;
  } else {
    retainedEvents[eventParams->event] = NULL;
  }
  event_listener_t *ev = firstListener;
  while (ev != NULL) {
    if ((ev->eventMask[eventParams->event / 32] &
         (1UL << (eventParams->event % 32)))) {
      prvSendEvent(ev, eventParams);
    }
    ev = ev->next;
  }
  /* If no subscribers, make sure event is freed */
  if (eventParams->dynamicAlloc && eventParams->refCount == 0) {
    vTaskSuspendAll();
    switch (eventParams->dynamicAlloc) {
    case DYN_ALLOC_SMALL:
      mp_free(&mpSmall, eventParams);
      break;
    case DYN_ALLOC_MED:
      mp_free(&mpMed, eventParams);
      break;
    case DYN_ALLOC_LARGE:
      mp_free(&mpLarge, eventParams);
      break;
    default:
      break;
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
  if (firstListener == NULL) {
    firstListener = listener;
    firstListener->prev = NULL;
    firstListener->next = NULL;
  } else {
    // Walk the list
    ev = firstListener;
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
      firstListener = NULL;
    } else {
      firstListener = listener->next;
      firstListener->prev = NULL;
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

static void prvInvdaliteEvent(event_t *event) {
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
  if (listener->queueHandle != NULL) {
    configASSERT(uxTaskPriorityGet(NULL) < EVENT_BUS_RTOS_PRIORITY);
  }
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

void publishEvent(event_t *ev, bool retain) {
  configASSERT(ev);
  configASSERT(ev->event < EVENT_BUS_BITS);
  /* Retained events must be statically allocated */
  configASSERT(retain ? ev->dynamicAlloc == 0 : 1);
  EVENT_CMD cmd = {.command = CMD_NEW_EVENT, .eventData = ev, .params = retain};
  cmd.xCallingTask = xTaskGetCurrentTaskHandle();
  xQueueSendToBack(xQueueCmd, (void *)&cmd, portMAX_DELAY);
#ifdef EVENT_BUS_USE_TASK_NOTIFICATION_INDEX
  ulTaskNotifyTakeIndexed(EVENT_BUS_USE_TASK_NOTIFICATION_INDEX, pdTRUE,
                          portMAX_DELAY);
#else
  taskYIELD();
#endif
}

BaseType_t publishEventFromISR(event_t *ev) {
  configASSERT(ev);
  configASSERT(ev->event < EVENT_BUS_BITS);
  EVENT_CMD cmd = {
      .command = CMD_NEW_EVENT, .eventData = ev, .params = 0};
  cmd.xCallingTask = NULL;
  return xQueueSendToBackFromISR(xQueueCmd, (void *)&cmd, NULL) == pdTRUE;
}

BaseType_t publishToListener(event_listener_t *listener, event_t *ev,
                             TickType_t xTicksToWait) {
  configASSERT(ev);
  configASSERT(listener);
  configASSERT(listener->queueHandle);
  vTaskSuspendAll(); /* May not be necessary */
  if (ev->dynamicAlloc) {
    ev->refCount++;
    listener->refCount++;
  }
  xTaskResumeAll();
  return xQueueSendToBack(listener->queueHandle, &ev, xTicksToWait);
}

void invalidateEvent(event_t *ev) {
  configASSERT(ev);
  EVENT_CMD cmd = {.command = CMD_INVALIDATE_EVENT, .eventData = ev};
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
#if EVENT_BUS_DYNAMIC_FREERTOS == 1
  (void)xTaskCreate(eventBusTasks, "Event-Bus", STACK_SIZE, NULL, EVENT_BUS_RTOS_PRIORITY, &processHandle);
  xQueueCmd = xQueueCreate(EVENT_BUS_MAX_CMD_QUEUE, sizeof(EVENT_CMD));
#else
  processHandle =
      xTaskCreateStatic(eventBusTasks, "Event-Bus", STACK_SIZE, NULL,
          EVENT_BUS_RTOS_PRIORITY, xStack, &xTaskBuffer);
  xQueueCmd = xQueueCreateStatic(EVENT_BUS_MAX_CMD_QUEUE, sizeof(EVENT_CMD),
      ucQueueStorage, &xStaticQueue);
#endif

  mp_init(POOL_SIZE_CALC(EVENT_BUS_POOL_SM_SZ), EVENT_BUS_POOL_SM_CT,
          smEventPool, &mpSmall);
  mp_init(POOL_SIZE_CALC(EVENT_BUS_POOL_MD_SZ), EVENT_BUS_POOL_MD_CT,
          mdEventPool, &mpMed);
  mp_init(POOL_SIZE_CALC(EVENT_BUS_POOL_LG_SZ), EVENT_BUS_POOL_LG_CT,
          lgEventPool, &mpLarge);
  return processHandle;
}

void *eventAlloc(size_t size, uint32_t eventId, uint16_t publisherId) {
  event_t *val;
  DYN_ALLOC_T dyn = DYN_ALLOC_NONE;
  configASSERT(size >= sizeof(event_t));
  configASSERT(size <= POOL_SIZE_CALC(EVENT_BUS_POOL_LG_SZ));
  vTaskSuspendAll();
  if (size <= POOL_SIZE_CALC(EVENT_BUS_POOL_SM_SZ)) {
    val = mp_malloc(&mpSmall);
    dyn = DYN_ALLOC_SMALL;
  } else if (size <= POOL_SIZE_CALC(EVENT_BUS_POOL_MD_SZ)) {
    val = mp_malloc(&mpMed);
    dyn = DYN_ALLOC_MED;
  } else if (size <= POOL_SIZE_CALC(EVENT_BUS_POOL_LG_SZ)) {
    val = mp_malloc(&mpLarge);
    dyn = DYN_ALLOC_LARGE;
  } else {
    configASSERT(0); /* Size not allowed */
  }
  configASSERT(val);
  val->dynamicAlloc = dyn;
  val->refCount = 0;
  val->event = eventId;
  val->publisherId = publisherId;
  xTaskResumeAll();
  return (void *)val;
}

void eventRelease(event_t *ev, event_listener_t *listener) {
  configASSERT(ev);
  configASSERT(listener);
  uint32_t evResponse;
  vTaskSuspendAll();
  if (ev->dynamicAlloc) {
    configASSERT(ev->refCount > 0);       /* Too many releases */
    configASSERT(listener->refCount > 0); /* NOTE: Too many releases */
    ev->refCount--;
    listener->refCount--;
    if (ev->refCount == 0) {
      if (ev->event < EVENT_BUS_BITS && ev->published) {
        evResponse = EVENT_BUS_TIME_SOURCE - ev->publishTime;
        if (evResponse > eventMaxResponse[ev->event]) {
          eventMaxResponse[ev->event] = evResponse;
        }
        if (evResponse < eventMinResponse[ev->event] || !eventMinResponse[ev->event]) {
          eventMinResponse[ev->event] = evResponse;
        }
      }
      switch (ev->dynamicAlloc) {
      default:
        configASSERT(ev->dynamicAlloc <= DYN_ALLOC_LARGE);
        break;
      case DYN_ALLOC_SMALL:
        mp_free(&mpSmall, ev);
        break;
      case DYN_ALLOC_MED:
        mp_free(&mpMed, ev);
        break;
      case DYN_ALLOC_LARGE:
        mp_free(&mpLarge, ev);
        break;
      }
    }
  }
  xTaskResumeAll();
}

uint32_t eventListenerInfo(char * const buf, uint32_t bufLen) {
  uint32_t pLen;
  if (firstListener == NULL) {
    return snprintf(buf, bufLen, "No registered events");
  }
  pLen = snprintf(buf, bufLen, "Name       Refs\r\n");
  if (pLen >= bufLen) {
    return bufLen;
  }
  event_listener_t *ev = firstListener;
  vTaskSuspendAll();
  while (ev != NULL) {
    pLen += snprintf(&buf[pLen], bufLen - pLen, " %-10s %2i\r\n", ev->name,
                     ev->refCount);
    if (pLen >= bufLen) {
      xTaskResumeAll();
      return bufLen;
    }
    ev = ev->next;
  }
  xTaskResumeAll();
  return pLen;
}

uint32_t eventResponseInfo(char *const buf, uint32_t bufLen) {
  uint32_t pLen;
  uint32_t i;
  pLen = snprintf(buf, bufLen, "ID      min       max\r\n");
  if (pLen >= bufLen) {
    return bufLen;
  }
  vTaskSuspendAll();
  for (i = 0; i < EVENT_BUS_BITS; i++) {
    if (eventMinResponse[i] || eventMaxResponse[i]) {
      pLen += snprintf(&buf[pLen], bufLen - pLen, "%2i  %4i.%03i  %4i.%03i\r\n", i,
          eventMinResponse[i] / EVENT_BUS_TIME_DIV_US / 1000,
          eventMinResponse[i] / EVENT_BUS_TIME_DIV_US % 1000,
          eventMaxResponse[i] / EVENT_BUS_TIME_DIV_US / 1000,
          eventMaxResponse[i] / EVENT_BUS_TIME_DIV_US % 1000);
      if (pLen >= bufLen) {
        xTaskResumeAll();
        return bufLen;
      }
      eventMinResponse[i] = eventMaxResponse[i] = 0;
    }
  }
  xTaskResumeAll();
  return pLen;
}

uint32_t eventPoolInfo(char *const buf, uint32_t bufLen) {
  uint32_t pLen;
  mp_info_t info1, info2, info3;
  uint32_t res1, res2, res3;
  vTaskSuspendAll();
  res1 = mp_integrity(&mpSmall, &info1);
  res2 = mp_integrity(&mpMed, &info2);
  res3 = mp_integrity(&mpLarge, &info3);
  xTaskResumeAll();
  pLen = snprintf(buf, bufLen, "Pool   Used  Free / Total  Max  Size  Valid\r\n");
  if (pLen >= bufLen) {
    return bufLen;
  }
  pLen += snprintf(&buf[pLen], bufLen - pLen,
                   " Small %4i  %4i / %4i  %4i  %4i  %4s\r\n", info1.count,
                   info1.freeCount, info1.blockCount, info1.high_water,
                   EVENT_BUS_POOL_SM_SZ, res1 ? "YES" : "NO");
  if (pLen >= bufLen) {
    return bufLen;
  }
  pLen += snprintf(&buf[pLen], bufLen - pLen,
                   " Med   %4i  %4i / %4i  %4i  %4i  %4s\r\n", info2.count,
                   info2.freeCount, info2.blockCount, info2.high_water,
                   EVENT_BUS_POOL_MD_SZ, res2 ? "YES" : "NO");
  if (pLen >= bufLen) {
    return bufLen;
  }
  pLen += snprintf(&buf[pLen], bufLen - pLen,
                   " Large %4i  % 4i / %4i  %4i  %4i  %4s\r\n", info3.count,
                   info3.freeCount, info3.blockCount, info3.high_water,
                   EVENT_BUS_POOL_LG_SZ, res3 ? "YES" : "NO");
  if (pLen >= bufLen) {
    return bufLen;
  }
  return pLen;
}