/*----------------------------------------------------------------------------/
/ TJpgDec - Tiny JPEG Decompressor R0.03 include file
/----------------------------------------------------------------------------*/
#ifndef TJPGDEC_H
#define TJPGDEC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error code */
typedef enum {
    JDR_OK = 0, /* 0: Succeeded */
    JDR_INTR,   /* 1: Interrupted by output function */
    JDR_INVAL,  /* 2: Device error or wrong termination */
    JDR_MEM1,   /* 3: Insufficient memory for LOU */
    JDR_MEM2,   /* 4: Insufficient stream buffer */
    JDR_PAR,    /* 5: Parameter error */
    JDR_FMT1,   /* 6: Data format error (may be damaged) */
    JDR_FMT2,   /* 7: Right format but not supported */
    JDR_FMT3    /* 8: Not supported JPEG standard */
} JRESULT;

/* Rectangular structure */
typedef struct {
    uint16_t left, right, top, bottom;
} JRECT;

/* Decompressor object structure */
typedef struct JDEC JDEC;
struct JDEC {
    uint16_t dctr;          /* Number of bytes available in the input buffer */
    uint8_t* dptr;          /* Current read pointer */
    uint8_t* inbuf;         /* Bit stream input buffer */
    uint8_t dbuf[256];      /* Input stream cache */
    uint8_t wbyte;          /* Work byte for bit stream */
    uint8_t wbit;           /* Bit count in wbyte */
    uint8_t scale;          /* Scaling factor */
    uint8_t msx, msy;       /* Sampling factor of Y (H,V) */
    uint8_t qtid[3];        /* Quantization table ID of Y, Cb, Cr */
    int16_t dqt[3][64];     /* Quantization tables */
    int16_t huff[2][2][162];/* Huffman tables [Y/C][DC/AC] */
    int16_t mcu_pre[3];     /* Previous DC element */
    void* device;           /* User defined device identifier */
    uint16_t width, height; /* Size of the input image (pixels) */
    uint8_t* mcu_buf;       /* Working buffer for MCU extraction */
    void* pool;             /* Pointer to available memory pool */
    uint16_t sz_pool;       /* Size of memory pool (bytes) */
    uint16_t (*infunc)(JDEC*, uint8_t*, uint16_t); /* Input function */
};

/* TJpgDec API functions */
JRESULT jd_prepare (JDEC* jd, uint16_t (*infunc)(JDEC*, uint8_t*, uint16_t), void* pool, uint16_t sz_pool, void* dev);
JRESULT jd_decomp (JDEC* jd, uint16_t (*outfunc)(JDEC*, void*, JRECT*), uint8_t scale);

#ifdef __cplusplus
}
#endif

#endif
