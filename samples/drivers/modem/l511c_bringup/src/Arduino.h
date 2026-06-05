/*
 * Minimal Arduino compatibility shim for Zephyr RTOS
 * Provides the bare minimum for MicroOcpp to compile.
 */
#ifndef ARDUINO_COMPAT_H
#define ARDUINO_COMPAT_H

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cstdio>

/* Arduino types */
typedef uint8_t byte;
typedef bool boolean;
#define String char /* not a real String, just for type compatibility */

/* Time */
inline unsigned long millis(void) { return (unsigned long)k_uptime_get(); }
inline unsigned long micros(void) { return (unsigned long)k_uptime_ticks() * 1000 / CONFIG_SYS_CLOCK_TICKS_PER_SEC; }
inline void delay(unsigned long ms) { k_msleep(ms); }

/* Serial stub */
#define Serial 0

/* Print format macros */
#define PSTR(x) x
#define F(x) x

/* Arduino pin functions (stubs) */
#define OUTPUT 1
#define INPUT 0
#define HIGH 1
#define LOW 0
inline void pinMode(int pin, int mode) {}
inline void digitalWrite(int pin, int val) {}
inline int digitalRead(int pin) { return 0; }

/* Memory */
#define PROGMEM

/* Standard functions Arduino usually provides */
inline char *itoa(int val, char *s, int radix) {
    if (radix == 10) { snprintf(s, 16, "%d", val); }
    else if (radix == 16) { snprintf(s, 16, "%x", val); }
    else if (radix == 8) { snprintf(s, 16, "%o", val); }
    return s;
}

inline char *ltoa(long val, char *s, int radix) {
    if (radix == 10) { snprintf(s, 32, "%ld", val); }
    return s;
}

inline char *ultoa(unsigned long val, char *s, int radix) {
    if (radix == 10) { snprintf(s, 32, "%lu", val); }
    return s;
}

/* dtostrf - float to string (Arduino style) */
inline char *dtostrf(double val, int width, int prec, char *s) {
    snprintf(s, width + 2, "%*.*f", width, prec, val);
    return s;
}

#endif /* ARDUINO_COMPAT_H */
