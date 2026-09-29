/*
 * archie_time.h - Base dei tempi della macchina Archimedes.
 *
 * Tutti i chip misurano il tempo nella stessa unita': un periodo del
 * quarzo principale a 24 MHz (41,67 ns). Da li' derivano tutti i clock:
 *   CPU e MEMC a 8 MHz  = 3 unita'
 *   WD1772 a 8 MHz      = 3 unita'
 *   timer dell'IOC 2 MHz = 12 unita'
 *   pixel del VIDC: 24, 16, 12 o 8 MHz = 1, 1.5, 2 o 3 unita'
 */
#ifndef ARCHIE_TIME_H
#define ARCHIE_TIME_H

#include <stdint.h>

typedef uint64_t ArcTime;

#define ARC_HZ          24000000ull
#define ARC_US(us)      ((ArcTime)(us) * 24u)
#define ARC_MS(ms)      ((ArcTime)(ms) * 24000u)

#endif
