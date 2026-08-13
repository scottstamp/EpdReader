/*----------------------------------------------------------------------------/
/ TJpgDec - Tiny JPEG Decompressor R0.03
/----------------------------------------------------------------------------*/
#include "tjpgd.h"
#include <string.h>

#define JD_SZBUF 512

/* Zigzag table */
static const uint8_t Zig[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

static uint8_t byte_pick(JDEC* jd) {
    if (jd->dctr == 0) {
        jd->dctr = jd->infunc(jd, jd->dbuf, JD_SZBUF);
        if (jd->dctr == 0) return 0;
        jd->dptr = jd->dbuf;
    }
    jd->dctr--;
    return *jd->dptr++;
}

static JRESULT create_qt(JDEC* jd, uint8_t* p, uint16_t len) {
    while (len >= 65) {
        uint8_t id = *p++;
        if (id & 0xF0) return JDR_FMT1;
        id &= 0x0F;
        if (id > 3) return JDR_FMT1;
        for (int i = 0; i < 64; i++) {
            jd->dqt[id][Zig[i]] = *p++;
        }
        len -= 65;
    }
    return JDR_OK;
}

JRESULT jd_prepare(JDEC* jd, uint16_t (*infunc)(JDEC*, uint8_t*, uint16_t), void* pool, uint16_t sz_pool, void* dev) {
    memset(jd, 0, sizeof(JDEC));
    jd->infunc = infunc;
    jd->pool = pool;
    jd->sz_pool = sz_pool;
    jd->device = dev;

    uint8_t seg[2];
    if (infunc(jd, seg, 2) != 2 || seg[0] != 0xFF || seg[1] != 0xD8) {
        return JDR_FMT1; // Not SOI
    }

    while (1) {
        if (infunc(jd, seg, 2) != 2 || seg[0] != 0xFF) return JDR_FMT1;
        uint8_t marker = seg[1];
        if (marker == 0xC0) { // SOF0
            uint8_t buf[9];
            if (infunc(jd, buf, 9) != 9) return JDR_FMT1;
            jd->height = (buf[1] << 8) | buf[2];
            jd->width = (buf[3] << 8) | buf[4];
            return JDR_OK;
        } else if (marker == 0xDB) { // DQT
            uint16_t len = (byte_pick(jd) << 8) | byte_pick(jd);
            if (len < 2) return JDR_FMT1;
            len -= 2;
            uint8_t dqt_buf[130];
            if (len > sizeof(dqt_buf)) len = sizeof(dqt_buf);
            infunc(jd, dqt_buf, len);
            create_qt(jd, dqt_buf, len);
        } else if (marker == 0xD9 || marker == 0xDA) {
            break;
        } else {
            uint16_t len = (byte_pick(jd) << 8) | byte_pick(jd);
            if (len < 2) return JDR_FMT1;
            len -= 2;
            while (len--) byte_pick(jd);
        }
    }
    return JDR_OK;
}

JRESULT jd_decomp(JDEC* jd, uint16_t (*outfunc)(JDEC*, void*, JRECT*), uint8_t scale) {
    if (!jd || !outfunc) return JDR_PAR;
    jd->scale = scale;
    return JDR_OK;
}
