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

#define EVENT_BUS_FLAGS_NONE       ( 0 )
#define EVENT_BUS_FLAGS_RETAIN     (1U << 0)

#define EVENT_BUS_BITS 32

#include "FreeRTOS.h"
#include "task.h"
#include "event_bus_config.h"
#include "message_buffer.h"

typedef struct {
  uint32_t event;
  uint16_t len;
  uint16_t flags;
  void *ptr;
  MessageBufferHandle_t ignore;
} event_params_t;

typedef struct {
  uint32_t event;
  uint16_t len;
  uint16_t flags;
  uint8_t data[EVENT_BUS_MAX_DATA_LEN];
} event_msg_t;

struct EVENT_T {
  uint32_t eventMask;
  MessageBufferHandle_t msgBuffHandle;
  uint32_t errFull : 1;
  struct EVENT_T *prev;
  struct EVENT_T *next;
};
typedef struct EVENT_T event_t;

void initEventBus(void);
void subscribeEvent(event_t *event);
void unSubscribeEvent(event_t *event);
void publishEvent(event_params_t *event);
void invalidateEvent(event_params_t *event);
TaskHandle_t eventBusProcessHandle(void);

#endif /* EVENTBUS_H */

