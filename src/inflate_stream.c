/* inflate_stream.c
 * Streaming variant of puff (Copyright (C) 2002-2013 Mark Adler, zlib license).
 * Output goes into a 32 KB ring buffer that is flushed in 16 KB halves, so
 * back-references up to the Deflate maximum of 32768 bytes stay valid.
 */

#include "inflate_stream.h"

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define MAXCODES (MAXLCODES + MAXDCODES)
#define FIXLCODES 288

#define WIN_MASK (INFLATE_WINDOW_SIZE - 1)
#define FLUSH_CHUNK (INFLATE_WINDOW_SIZE / 2)

#define ERR_INPUT 2
#define ERR_WRITE 4

struct state {
    uint8_t *win;
    uint32_t outcnt;   // total bytes produced
    uint32_t flushed;  // total bytes handed to wr()

    uint8_t *in;
    size_t insize;
    size_t inlen;
    size_t incnt;
    int bitbuf;
    int bitcnt;
    int error;

    inflate_read_fn rd;
    inflate_write_fn wr;
    void *ctx;
};

static int next_byte(struct state *s) {
    if (s->incnt >= s->inlen) {
        int n = s->rd(s->ctx, s->in, s->insize);
        if (n <= 0) {
            s->error = ERR_INPUT;
            return -1;
        }
        s->inlen = (size_t)n;
        s->incnt = 0;
    }
    return s->in[s->incnt++];
}

static void flush_out(struct state *s) {
    uint32_t pending = s->outcnt - s->flushed;
    if (pending == 0 || s->error) return;
    if (s->wr(s->ctx, &s->win[s->flushed & WIN_MASK], pending) != 0) {
        s->error = ERR_WRITE;
    }
    s->flushed = s->outcnt;
}

static void put_byte(struct state *s, uint8_t b) {
    s->win[s->outcnt & WIN_MASK] = b;
    s->outcnt++;
    // Chunks start on FLUSH_CHUNK boundaries, so each flush is contiguous in the ring
    if (s->outcnt - s->flushed == FLUSH_CHUNK) {
        flush_out(s);
    }
}

static int bits(struct state *s, int need) {
    long val = s->bitbuf;
    if (s->error) return 0;
    while (s->bitcnt < need) {
        int b = next_byte(s);
        if (b < 0) return 0;
        val |= (long)b << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = (int)(val >> need);
    s->bitcnt -= need;
    return (int)(val & ((1L << need) - 1));
}

static int stored(struct state *s) {
    unsigned len, nlen;
    int b0, b1, b2, b3;

    s->bitbuf = 0;
    s->bitcnt = 0;
    b0 = next_byte(s);
    b1 = next_byte(s);
    b2 = next_byte(s);
    b3 = next_byte(s);
    if (s->error) return s->error;
    len = (unsigned)b0 | ((unsigned)b1 << 8);
    nlen = (unsigned)b2 | ((unsigned)b3 << 8);
    if (len != (~nlen & 0xffff)) return -2;
    while (len--) {
        int b = next_byte(s);
        if (b < 0) return s->error;
        put_byte(s, (uint8_t)b);
        if (s->error) return s->error;
    }
    return 0;
}

struct huffman {
    short *count;
    short *symbol;
};

static int decode(struct state *s, const struct huffman *h) {
    int len;
    int code = 0;
    int first = 0;
    int count;
    int index = 0;

    for (len = 1; len <= MAXBITS; len++) {
        code |= bits(s, 1);
        if (s->error) return -10;
        count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -10;
}

static int construct(struct huffman *h, const short *length, int n) {
    int symbol;
    int len;
    int left;
    short offs[MAXBITS + 1];

    for (len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
    if (h->count[0] == n) return 0;

    left = 1;
    for (len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }

    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++)
        offs[len + 1] = offs[len] + h->count[len];

    for (symbol = 0; symbol < n; symbol++)
        if (length[symbol] != 0)
            h->symbol[offs[length[symbol]]++] = symbol;

    return left;
}

static int codes(struct state *s, const struct huffman *lencode, const struct huffman *distcode) {
    int symbol;
    int len;
    unsigned dist;
    static const short lens[] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const short lext[] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const short dists[] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
        8193, 12289, 16385, 24577
    };
    static const short dext[] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };

    do {
        symbol = decode(s, lencode);
        if (s->error) return s->error;
        if (symbol < 0) return symbol;
        if (symbol < 256) {
            put_byte(s, (uint8_t)symbol);
            if (s->error) return s->error;
        } else if (symbol > 256) {
            symbol -= 257;
            if (symbol >= 29) return -10;
            len = lens[symbol] + bits(s, lext[symbol]);
            if (s->error) return s->error;
            symbol = decode(s, distcode);
            if (s->error) return s->error;
            if (symbol < 0) return symbol;
            if (symbol >= 30) return -10;
            dist = dists[symbol] + bits(s, dext[symbol]);
            if (s->error) return s->error;
            if (dist > s->outcnt || dist > INFLATE_WINDOW_SIZE) return -3;
            while (len--) {
                put_byte(s, s->win[(s->outcnt - dist) & WIN_MASK]);
                if (s->error) return s->error;
            }
        }
    } while (symbol != 256);

    return 0;
}

static int fixed(struct state *s) {
    static int virgin = 1;
    static short lencnt[MAXBITS + 1], lensym[FIXLCODES];
    static short distcnt[MAXBITS + 1], distsym[MAXDCODES];
    static struct huffman lencode = {lencnt, lensym};
    static struct huffman distcode = {distcnt, distsym};

    if (virgin) {
        int symbol;
        short lengths[FIXLCODES];

        for (symbol = 0; symbol < 144; symbol++) lengths[symbol] = 8;
        for (; symbol < 256; symbol++) lengths[symbol] = 9;
        for (; symbol < 280; symbol++) lengths[symbol] = 7;
        for (; symbol < FIXLCODES; symbol++) lengths[symbol] = 8;
        construct(&lencode, lengths, FIXLCODES);

        for (symbol = 0; symbol < MAXDCODES; symbol++) lengths[symbol] = 5;
        construct(&distcode, lengths, MAXDCODES);

        virgin = 0;
    }

    return codes(s, &lencode, &distcode);
}

static int dynamic(struct state *s) {
    int nlen, ndist, ncode;
    int index, err;
    short lengths[MAXCODES];

    short lencnt[MAXBITS + 1], lensym[MAXLCODES];
    short distcnt[MAXBITS + 1], distsym[MAXDCODES];
    struct huffman lencode, distcode;
    static const short order[] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    lencode.count = lencnt;
    lencode.symbol = lensym;
    distcode.count = distcnt;
    distcode.symbol = distsym;

    nlen = bits(s, 5) + 257;
    ndist = bits(s, 5) + 1;
    ncode = bits(s, 4) + 4;

    if (s->error) return s->error;
    if (nlen > MAXLCODES || ndist > MAXDCODES) return -3;

    for (index = 0; index < ncode; index++) {
        lengths[order[index]] = bits(s, 3);
        if (s->error) return s->error;
    }
    for (; index < 19; index++)
        lengths[order[index]] = 0;

    err = construct(&lencode, lengths, 19);
    if (err != 0) return -4;

    index = 0;
    while (index < nlen + ndist) {
        int symbol;

        symbol = decode(s, &lencode);
        if (s->error) return s->error;
        if (symbol < 0) return symbol;

        if (symbol < 16) {
            lengths[index++] = symbol;
        } else {
            int len = 0;

            if (symbol == 16) {
                if (index == 0) return -5;
                len = lengths[index - 1];
                symbol = 3 + bits(s, 2);
            } else if (symbol == 17) {
                symbol = 3 + bits(s, 3);
            } else {
                symbol = 11 + bits(s, 7);
            }

            if (s->error) return s->error;
            if (index + symbol > nlen + ndist) return -6;
            while (symbol--) lengths[index++] = len;
        }
    }

    if (lengths[256] == 0) return -9;

    // Incomplete codes are only allowed for a single length-1 code
    err = construct(&lencode, lengths, nlen);
    if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1]))
        return -7;

    err = construct(&distcode, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1]))
        return -8;

    return codes(s, &lencode, &distcode);
}

int inflate_stream(uint8_t *window, uint8_t *inbuf, size_t inbuf_size,
                   inflate_read_fn rd, inflate_write_fn wr, void *ctx) {
    struct state s;
    int last, type;
    int err = 0;

    s.win = window;
    s.outcnt = 0;
    s.flushed = 0;
    s.in = inbuf;
    s.insize = inbuf_size;
    s.inlen = 0;
    s.incnt = 0;
    s.bitbuf = 0;
    s.bitcnt = 0;
    s.error = 0;
    s.rd = rd;
    s.wr = wr;
    s.ctx = ctx;

    do {
        last = bits(&s, 1);
        type = bits(&s, 2);
        if (s.error) {
            err = s.error;
            break;
        }
        switch (type) {
            case 0: err = stored(&s); break;
            case 1: err = fixed(&s); break;
            case 2: err = dynamic(&s); break;
            default: err = -1; break;
        }
        if (err != 0) break;
    } while (!last);

    // Hand over whatever was decoded, even after an error, so a damaged
    // entry still yields its readable prefix
    if (err != ERR_WRITE) {
        s.error = 0;
        flush_out(&s);
        if (s.error) err = ERR_WRITE;
    }
    return err;
}
