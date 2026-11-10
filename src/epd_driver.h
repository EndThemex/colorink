#ifndef INKSIGHT_EPD_DRIVER_H
#define INKSIGHT_EPD_DRIVER_H

#include <Arduino.h>

// Initialize GPIO pins and SPI for EPD
void gpioInit();

// Initialize EPD controller (hardware reset + register setup + power on).
// Each refresh runs this first, so a deep-sleeped controller is woken here.
void epdInit();

// Display a pre-packed 2bpp (BWRY) framebuffer (4 px/byte: 00=black 01=white
// 10=yellow 11=red). Blocks ~15s. Retries up to 3 times on busy timeout.
// Returns false if the panel failed to complete the refresh (or another
// refresh is already in progress).
bool epdDisplay2bpp(const uint8_t *image2bpp);

// True while a panel refresh is in flight (~15s). Callers must not start a
// second refresh concurrently — the panel state would be corrupted.
bool epdIsRendering();

// Put the EPD controller into deep sleep (idempotent). A hardware reset
// (epdInit -> epdReset) is required before the next refresh.
void epdSleep();

#endif // INKSIGHT_EPD_DRIVER_H
