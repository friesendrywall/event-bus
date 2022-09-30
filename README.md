# event-bus
Smallish event bus for FreeRTOS based on message buffers.  Receiving task must set up
a FreeRTOS xMessageBufferCreate*() in order to receive these messages.

Supports up to 32 bitmasked events.

Test with Visual Studio 2019 or greater.