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

#ifndef EVENTBUS_H
#define EVENTBUS_H

#define EVENT_BUS_FLAGS_RETAIN (1UL << 0)
#define EVENT_BUS_BITS (32 * EVENT_BUS_MASK_WIDTH)
#define EVENT_BUS_LAST_PARAM (EVENT_BUS_BITS + 1)

#include <stdbool.h>
#include <stdarg.h>

#include "event_bus_config.h"
#include <FreeRTOS.h>
#include <task.h>

#ifndef EVENT_BUS_MASK_WIDTH
#error EVENT_BUS_MASK_WIDTH must be declared in config file
#endif

typedef struct {
  uint32_t event;
  union {
    void *ptr;
    uint32_t value;
  };
  uint16_t len;
  uint16_t publisherId;
} event_params_t;

typedef void (*eventCallback)(event_params_t *);

struct EVENT_T {
  uint32_t eventMask[EVENT_BUS_MASK_WIDTH];
  eventCallback callback;
  struct EVENT_T *prev;
  struct EVENT_T *next;
};
typedef struct EVENT_T event_t;

TaskHandle_t initEventBus(void);
void subEvent(event_t *event, uint32_t eventId);
void subEventList(event_t *event, const uint32_t *eventList);
void unSubEvent(event_t *event, uint32_t eventId);
void attachBus(event_t *event);
void detachBus(event_t *event);
void publishEvent(event_params_t *event, bool retain);
BaseType_t publishEventFromISR(event_params_t *event);
void publishEventQ(uint32_t event, uint32_t value);
void invalidateEvent(event_params_t *event);

#endif /* EVENTBUS_H */