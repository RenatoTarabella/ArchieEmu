/* Ingrandimento ad area per fattori non interi: ogni punto dello schermo
   prende i pixel sorgente che copre in proporzione alla superficie, cosi'
   le linee da un pixel restano tutte dello stesso spessore. */
#ifndef SCALE_AREA_H
#define SCALE_AREA_H

#include <stdint.h>

/* src: w x h pixel XRGB con passo src_stride; restituisce un buffer interno
   sw x sh (passo sw), valido fino alla chiamata successiva, o NULL se manca
   memoria. Pensata per sw >= w e sh >= h. */
const uint32_t *scale_area(const uint32_t *src, int src_stride, int w, int h, int sw, int sh);

#endif
