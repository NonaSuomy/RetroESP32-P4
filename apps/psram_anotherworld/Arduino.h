#pragma once

/* Tiny compatibility surface for the upstream source files. Hardware timing
 * and serial output are supplied by the PAPP system wrapper; no Arduino or
 * VGA driver is linked into the app. */
unsigned long millis(void);
void delay(unsigned long milliseconds);

class PappSerial {
public:
    template <typename... Args>
    int printf(const char *, Args...) { return 0; }
};

extern PappSerial Serial;
