/*
 * Copyright (c) 2010-2011 Maxim Poliakovski
 * Copyright (c) 2010-2011 Elvis Presley
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Known FOURCCs: 'apch' (HQ), 'apcn' (SD), 'apcs' (LT), 'apco' (Proxy), 'ap4h' (4444), 'ap4x' (4444 XQ)
 */

//#define DEBUG

#include "config_components.h"

#include "libavutil/internal.h"
#include "libavutil/mem.h"
#include "libavutil/mem_internal.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "get_bits.h"
#include "hwaccel_internal.h"
#include "hwconfig.h"
#include "idctdsp.h"
#include "profiles.h"
#include "proresdec.h"
#include "proresdata.h"
#include "thread.h"

#define ALPHA_SHIFT_16_TO_10(alpha_val) (alpha_val >> 6)
#define ALPHA_SHIFT_8_TO_10(alpha_val)  ((alpha_val << 2) | (alpha_val >> 6))
#define ALPHA_SHIFT_16_TO_12(alpha_val) (alpha_val >> 4)
#define ALPHA_SHIFT_8_TO_12(alpha_val)  ((alpha_val << 4) | (alpha_val >> 4))

static void inline unpack_alpha(GetBitContext *gb, uint16_t *dst, int num_coeffs,
                                const int num_bits, const int decode_precision) {
    const int mask = (1 << num_bits) - 1;
    int i, idx, val, alpha_val;

    idx       = 0;
    alpha_val = mask;
    do {
        do {
            if (get_bits1(gb)) {
                val = get_bits(gb, num_bits);
            } else {
                int sign;
                val  = get_bits(gb, num_bits == 16 ? 7 : 4);
                sign = val & 1;
                val  = (val + 2) >> 1;
                if (sign)
                    val = -val;
            }
            alpha_val = (alpha_val + val) & mask;
            if (num_bits == 16) {
                if (decode_precision == 10) {
                    dst[idx++] = ALPHA_SHIFT_16_TO_10(alpha_val);
                } else { /* 12b */
                    dst[idx++] = ALPHA_SHIFT_16_TO_12(alpha_val);
                }
            } else {
                if (decode_precision == 10) {
                    dst[idx++] = ALPHA_SHIFT_8_TO_10(alpha_val);
                } else { /* 12b */
                    dst[idx++] = ALPHA_SHIFT_8_TO_12(alpha_val);
                }
            }
            if (idx >= num_coeffs)
                break;
        } while (get_bits_left(gb)>0 && get_bits1(gb));
        val = get_bits(gb, 4);
        if (!val)
            val = get_bits(gb, 11);
        if (idx + val > num_coeffs)
            val = num_coeffs - idx;
        if (num_bits == 16) {
            for (i = 0; i < val; i++) {
                if (decode_precision == 10) {
                    dst[idx++] = ALPHA_SHIFT_16_TO_10(alpha_val);
                } else { /* 12b */
                    dst[idx++] = ALPHA_SHIFT_16_TO_12(alpha_val);
                }
            }
        } else {
            for (i = 0; i < val; i++) {
                if (decode_precision == 10) {
                    dst[idx++] = ALPHA_SHIFT_8_TO_10(alpha_val);
                } else { /* 12b */
                    dst[idx++] = ALPHA_SHIFT_8_TO_12(alpha_val);
                }
            }
        }
    } while (idx < num_coeffs);
}

static void unpack_alpha_10(GetBitContext *gb, uint16_t *dst, int num_coeffs,
                            const int num_bits)
{
    if (num_bits == 16) {
        unpack_alpha(gb, dst, num_coeffs, 16, 10);
    } else { /* 8 bits alpha */
        unpack_alpha(gb, dst, num_coeffs, 8, 10);
    }
}

static void unpack_alpha_12(GetBitContext *gb, uint16_t *dst, int num_coeffs,
                            const int num_bits)
{
    if (num_bits == 16) {
        unpack_alpha(gb, dst, num_coeffs, 16, 12);
    } else { /* 8 bits alpha */
        unpack_alpha(gb, dst, num_coeffs, 8, 12);
    }
}

static av_cold int decode_init(AVCodecContext *avctx)
{
    ProresContext *ctx = avctx->priv_data;

    avctx->bits_per_raw_sample = 10;

    switch (avctx->codec_tag) {
    case MKTAG('a','p','c','o'):
        avctx->profile = AV_PROFILE_PRORES_PROXY;
        break;
    case MKTAG('a','p','c','s'):
        avctx->profile = AV_PROFILE_PRORES_LT;
        break;
    case MKTAG('a','p','c','n'):
        avctx->profile = AV_PROFILE_PRORES_STANDARD;
        break;
    case MKTAG('a','p','c','h'):
        avctx->profile = AV_PROFILE_PRORES_HQ;
        break;
    case MKTAG('a','p','4','h'):
        avctx->profile = AV_PROFILE_PRORES_4444;
        avctx->bits_per_raw_sample = 12;
        break;
    case MKTAG('a','p','4','x'):
        avctx->profile = AV_PROFILE_PRORES_XQ;
        avctx->bits_per_raw_sample = 12;
        break;
    default:
        avctx->profile = AV_PROFILE_UNKNOWN;
        av_log(avctx, AV_LOG_WARNING, "Unknown prores profile %d\n", avctx->codec_tag);
    }

    ctx->unpack_alpha = avctx->bits_per_raw_sample == 10 ?
                            unpack_alpha_10 : unpack_alpha_12;

    av_log(avctx, AV_LOG_DEBUG,
           "Auto bitdepth precision. Use %db decoding based on codec tag.\n",
           avctx->bits_per_raw_sample);

    ff_blockdsp_init(&ctx->bdsp);
    ff_proresdsp_init(&ctx->prodsp, avctx->bits_per_raw_sample);

    ff_permute_scantable(ctx->progressive_scan, ff_prores_progressive_scan,
                         ctx->prodsp.idct_permutation);
    ff_permute_scantable(ctx->interlaced_scan,  ff_prores_interlaced_scan,
                         ctx->prodsp.idct_permutation);

    ctx->pix_fmt = AV_PIX_FMT_NONE;

    return 0;
}

static int decode_frame_header(ProresContext *ctx, const uint8_t *buf,
                               const int data_size, AVCodecContext *avctx)
{
    int hdr_size, width, height, flags;
    int version;
    const uint8_t *ptr;
    enum AVPixelFormat pix_fmt;

    hdr_size = AV_RB16(buf);
    ff_dlog(avctx, "header size %d\n", hdr_size);
    if (hdr_size > data_size) {
        av_log(avctx, AV_LOG_ERROR, "error, wrong header size\n");
        return AVERROR_INVALIDDATA;
    }

    version = AV_RB16(buf + 2);
    ff_dlog(avctx, "%.4s version %d\n", buf+4, version);
    if (version > 1) {
        av_log(avctx, AV_LOG_ERROR, "unsupported version: %d\n", version);
        return AVERROR_PATCHWELCOME;
    }

    width  = AV_RB16(buf + 8);
    height = AV_RB16(buf + 10);

    if (width != avctx->width || height != avctx->height) {
        int ret;

        av_log(avctx, AV_LOG_WARNING, "picture resolution change: %dx%d -> %dx%d\n",
               avctx->width, avctx->height, width, height);
        if ((ret = ff_set_dimensions(avctx, width, height)) < 0)
            return ret;
    }

    ctx->frame_type = (buf[12] >> 2) & 3;
    ctx->alpha_info = buf[17] & 0xf;

    if (ctx->alpha_info > 2) {
        av_log(avctx, AV_LOG_ERROR, "Invalid alpha mode %d\n", ctx->alpha_info);
        return AVERROR_INVALIDDATA;
    }
    if (avctx->skip_alpha) ctx->alpha_info = 0;

    ff_dlog(avctx, "frame type %d\n", ctx->frame_type);

    if (ctx->frame_type == 0) {
        ctx->scan = ctx->progressive_scan; // permuted
    } else {
        ctx->scan = ctx->interlaced_scan; // permuted
        ctx->frame->flags |= AV_FRAME_FLAG_INTERLACED;
        if (ctx->frame_type == 1)
            ctx->frame->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
    }

    if (ctx->alpha_info) {
        if (avctx->bits_per_raw_sample == 10) {
            pix_fmt = (buf[12] & 0xC0) == 0xC0 ? AV_PIX_FMT_YUVA444P10 : AV_PIX_FMT_YUVA422P10;
        } else { /* 12b */
            pix_fmt = (buf[12] & 0xC0) == 0xC0 ? AV_PIX_FMT_YUVA444P12 : AV_PIX_FMT_YUVA422P12;
        }
    } else {
        if (avctx->bits_per_raw_sample == 10) {
            pix_fmt = (buf[12] & 0xC0) == 0xC0 ? AV_PIX_FMT_YUV444P10 : AV_PIX_FMT_YUV422P10;
        } else { /* 12b */
            pix_fmt = (buf[12] & 0xC0) == 0xC0 ? AV_PIX_FMT_YUV444P12 : AV_PIX_FMT_YUV422P12;
        }
    }

    if (pix_fmt != ctx->pix_fmt) {
#define HWACCEL_MAX (CONFIG_PRORES_VIDEOTOOLBOX_HWACCEL)
#if HWACCEL_MAX
        enum AVPixelFormat pix_fmts[HWACCEL_MAX + 2], *fmtp = pix_fmts;
        int ret;

        ctx->pix_fmt = pix_fmt;

#if CONFIG_PRORES_VIDEOTOOLBOX_HWACCEL
        *fmtp++ = AV_PIX_FMT_VIDEOTOOLBOX;
#endif
        *fmtp++ = ctx->pix_fmt;
        *fmtp = AV_PIX_FMT_NONE;

        if ((ret = ff_get_format(avctx, pix_fmts)) < 0)
            return ret;

        avctx->pix_fmt = ret;
#else
        avctx->pix_fmt = ctx->pix_fmt = pix_fmt;
#endif
    }

    ctx->frame->color_primaries = buf[14];
    ctx->frame->color_trc       = buf[15];
    ctx->frame->colorspace      = buf[16];
    ctx->frame->color_range     = AVCOL_RANGE_MPEG;

    ptr   = buf + 20;
    flags = buf[19];
    ff_dlog(avctx, "flags %x\n", flags);

    if (flags & 2) {
        if(buf + data_size - ptr < 64) {
            av_log(avctx, AV_LOG_ERROR, "Header truncated\n");
            return AVERROR_INVALIDDATA;
        }
        ff_permute_scantable(ctx->qmat_luma, ctx->prodsp.idct_permutation, ptr);
        ptr += 64;
    } else {
        memset(ctx->qmat_luma, 4, 64);
    }

    if (flags & 1) {
        if(buf + data_size - ptr < 64) {
            av_log(avctx, AV_LOG_ERROR, "Header truncated\n");
            return AVERROR_INVALIDDATA;
        }
        ff_permute_scantable(ctx->qmat_chroma, ctx->prodsp.idct_permutation, ptr);
    } else {
        memcpy(ctx->qmat_chroma, ctx->qmat_luma, 64);
    }

    return hdr_size;
}

static int decode_picture_header(AVCodecContext *avctx, const uint8_t *buf, const int buf_size)
{
    ProresContext *ctx = avctx->priv_data;
    int i, hdr_size, slice_count;
    unsigned pic_data_size;
    int log2_slice_mb_width, log2_slice_mb_height;
    int slice_mb_count, mb_x, mb_y;
    const uint8_t *data_ptr, *index_ptr;

    hdr_size = buf[0] >> 3;
    if (hdr_size < 8 || hdr_size > buf_size) {
        av_log(avctx, AV_LOG_ERROR, "error, wrong picture header size\n");
        return AVERROR_INVALIDDATA;
    }

    pic_data_size = AV_RB32(buf + 1);
    if (pic_data_size > buf_size) {
        av_log(avctx, AV_LOG_ERROR, "error, wrong picture data size\n");
        return AVERROR_INVALIDDATA;
    }

    log2_slice_mb_width  = buf[7] >> 4;
    log2_slice_mb_height = buf[7] & 0xF;
    if (log2_slice_mb_width > 3 || log2_slice_mb_height) {
        av_log(avctx, AV_LOG_ERROR, "unsupported slice resolution: %dx%d\n",
               1 << log2_slice_mb_width, 1 << log2_slice_mb_height);
        return AVERROR_INVALIDDATA;
    }

    ctx->mb_width  = (avctx->width  + 15) >> 4;
    if (ctx->frame_type)
        ctx->mb_height = (avctx->height + 31) >> 5;
    else
        ctx->mb_height = (avctx->height + 15) >> 4;

    // QT ignores the written value
    // slice_count = AV_RB16(buf + 5);
    slice_count = ctx->mb_height * ((ctx->mb_width >> log2_slice_mb_width) +
                                    av_popcount(ctx->mb_width & (1 << log2_slice_mb_width) - 1));

    if (ctx->slice_count != slice_count || !ctx->slices) {
        av_freep(&ctx->slices);
        ctx->slice_count = 0;
        ctx->slices = av_calloc(slice_count, sizeof(*ctx->slices));
        if (!ctx->slices)
            return AVERROR(ENOMEM);
        ctx->slice_count = slice_count;
    }

    if (!slice_count)
        return AVERROR(EINVAL);

    if (hdr_size + slice_count*2 > buf_size) {
        av_log(avctx, AV_LOG_ERROR, "error, wrong slice count\n");
        return AVERROR_INVALIDDATA;
    }

    // parse slice information
    index_ptr = buf + hdr_size;
    data_ptr  = index_ptr + slice_count*2;

    slice_mb_count = 1 << log2_slice_mb_width;
    mb_x = 0;
    mb_y = 0;

    for (i = 0; i < slice_count; i++) {
        SliceContext *slice = &ctx->slices[i];

        slice->data = data_ptr;
        data_ptr += AV_RB16(index_ptr + i*2);

        while (ctx->mb_width - mb_x < slice_mb_count)
            slice_mb_count >>= 1;

        slice->mb_x = mb_x;
        slice->mb_y = mb_y;
        slice->mb_count = slice_mb_count;
        slice->data_size = data_ptr - slice->data;

        if (slice->data_size < 6) {
            av_log(avctx, AV_LOG_ERROR, "error, wrong slice data size\n");
            return AVERROR_INVALIDDATA;
        }

        mb_x += slice_mb_count;
        if (mb_x == ctx->mb_width) {
            slice_mb_count = 1 << log2_slice_mb_width;
            mb_x = 0;
            mb_y++;
        }
        if (data_ptr > buf + buf_size) {
            av_log(avctx, AV_LOG_ERROR, "error, slice out of bounds\n");
            return AVERROR_INVALIDDATA;
        }
    }

    if (mb_x || mb_y != ctx->mb_height) {
        av_log(avctx, AV_LOG_ERROR, "error wrong mb count y %d h %d\n",
               mb_y, ctx->mb_height);
        return AVERROR_INVALIDDATA;
    }

    return pic_data_size;
}

#define DECODE_CODEWORD(val, codebook, SKIP)                            \
    do {                                                                \
        unsigned int rice_order, exp_order, switch_bits;                \
        unsigned int q, buf, bits;                                      \
                                                                        \
        UPDATE_CACHE_32(re, gb); /* We really need 32 bits */           \
        buf = GET_CACHE(re, gb);                                        \
                                                                        \
        /* number of bits to switch between rice and exp golomb */      \
        switch_bits =  codebook & 3;                                    \
        rice_order  =  codebook >> 5;                                   \
        exp_order   = (codebook >> 2) & 7;                              \
                                                                        \
        q = 31 - av_log2(buf);                                          \
                                                                        \
        if (q > switch_bits) { /* exp golomb */                         \
            bits = exp_order - switch_bits + (q<<1);                    \
            if (bits > 31)                                              \
                return AVERROR_INVALIDDATA;                             \
            val = SHOW_UBITS(re, gb, bits) - (1 << exp_order) +         \
                ((switch_bits + 1) << rice_order);                      \
            SKIP(re, gb, bits);                                         \
        } else if (rice_order) {                                        \
            SKIP_BITS(re, gb, q+1);                                     \
            val = (q << rice_order) + SHOW_UBITS(re, gb, rice_order);   \
            SKIP(re, gb, rice_order);                                   \
        } else {                                                        \
            val = q;                                                    \
            SKIP(re, gb, q+1);                                          \
        }                                                               \
    } while (0)

#define TOSIGNED(x) (((x) >> 1) ^ (-((x) & 1)))

#define FIRST_DC_CB 0xB8

static const uint8_t dc_codebook[7] = { 0x04, 0x28, 0x28, 0x4D, 0x4D, 0x70, 0x70};

static av_always_inline int decode_dc_coeffs(GetBitContext *gb, int16_t *out,
                                              int blocks_per_slice)
{
    int16_t prev_dc;
    int code, i, sign;

    OPEN_READER(re, gb);

    DECODE_CODEWORD(code, FIRST_DC_CB, LAST_SKIP_BITS);
    prev_dc = TOSIGNED(code);
    out[0] = prev_dc;

    out += 64; // dc coeff for the next block

    code = 5;
    sign = 0;
    for (i = 1; i < blocks_per_slice; i++, out += 64) {
        DECODE_CODEWORD(code, dc_codebook[FFMIN(code, 6U)], LAST_SKIP_BITS);
        if(code) sign ^= -(code & 1);
        else     sign  = 0;
        prev_dc += (((code + 1) >> 1) ^ sign) - sign;
        out[0] = prev_dc;
    }
    CLOSE_READER(re, gb);
    return 0;
}

// adaptive codebook switching lut according to previous run/level values
static const uint8_t run_to_cb[16] = { 0x06, 0x06, 0x05, 0x05, 0x04, 0x29, 0x29, 0x29, 0x29, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x4C };
static const uint8_t lev_to_cb[10] = { 0x04, 0x0A, 0x05, 0x06, 0x04, 0x28, 0x28, 0x28, 0x28, 0x4C };

static av_always_inline int decode_ac_coeffs(AVCodecContext *avctx, GetBitContext *gb,
                                             int16_t *out, int blocks_per_slice)
{
    const ProresContext *ctx = avctx->priv_data;
    int block_mask, sign;
    unsigned pos, run, level;
    int max_coeffs, i, bits_left;
    int log2_block_count = av_log2(blocks_per_slice);

    OPEN_READER(re, gb);
    UPDATE_CACHE_32(re, gb);
    run   = 4;
    level = 2;

    max_coeffs = 64 << log2_block_count;
    block_mask = blocks_per_slice - 1;

    for (pos = block_mask;;) {
        bits_left = gb->size_in_bits - re_index;
        if (bits_left <= 0 || (bits_left < 32 && !SHOW_UBITS(re, gb, bits_left)))
            break;

        DECODE_CODEWORD(run, run_to_cb[FFMIN(run,  15)], LAST_SKIP_BITS);
        pos += run + 1;
        if (pos >= max_coeffs) {
            av_log(avctx, AV_LOG_ERROR, "ac tex damaged %d, %d\n", pos, max_coeffs);
            return AVERROR_INVALIDDATA;
        }

        DECODE_CODEWORD(level, lev_to_cb[FFMIN(level, 9)], SKIP_BITS);
        level += 1;

        i = pos >> log2_block_count;

        sign = SHOW_SBITS(re, gb, 1);
        SKIP_BITS(re, gb, 1);
        out[((pos & block_mask) << 6) + ctx->scan[i]] = ((level ^ sign) - sign);
    }

    CLOSE_READER(re, gb);
    return 0;
}

static int decode_slice_luma(AVCodecContext *avctx, SliceContext *slice,
                             uint16_t *dst, int dst_stride,
                             const uint8_t *buf, unsigned buf_size,
                             const int16_t *qmat)
{
    const ProresContext *ctx = avctx->priv_data;
    LOCAL_ALIGNED_32(int16_t, blocks, [8*4*64]);
    int16_t *block;
    GetBitContext gb;
    int i, blocks_per_slice = slice->mb_count<<2;
    int ret;

    for (i = 0; i < blocks_per_slice; i++)
        ctx->bdsp.clear_block(blocks+(i<<6));

    init_get_bits(&gb, buf, buf_size << 3);

    if ((ret = decode_dc_coeffs(&gb, blocks, blocks_per_slice)) < 0)
        return ret;
    if ((ret = decode_ac_coeffs(avctx, &gb, blocks, blocks_per_slice)) < 0)
        return ret;

    block = blocks;
    for (i = 0; i < slice->mb_count; i++) {
        ctx->prodsp.idct_put(dst, dst_stride, block+(0<<6), qmat);
        ctx->prodsp.idct_put(dst             +8, dst_stride, block+(1<<6), qmat);
        ctx->prodsp.idct_put(dst+4*dst_stride  , dst_stride, block+(2<<6), qmat);
        ctx->prodsp.idct_put(dst+4*dst_stride+8, dst_stride, block+(3<<6), qmat);
        block += 4*64;
        dst += 16;
    }
    return 0;
}

static int decode_slice_chroma(AVCodecContext *avctx, SliceContext *slice,
                               uint16_t *dst, int dst_stride,
                               const uint8_t *buf, unsigned buf_size,
                               const int16_t *qmat, int log2_blocks_per_mb)
{
    ProresContext *ctx = avctx->priv_data;
    LOCAL_ALIGNED_32(int16_t, blocks, [8*4*64]);
    int16_t *block;
    GetBitContext gb;
    int i, j, blocks_per_slice = slice->mb_count << log2_blocks_per_mb;
    int ret;

    for (i = 0; i < blocks_per_slice; i++)
        ctx->bdsp.clear_block(blocks+(i<<6));

    /* Some encodes have empty chroma scans to simulate grayscale */
    if (buf_size) {
        init_get_bits(&gb, buf, buf_size << 3);

        if ((ret = decode_dc_coeffs(&gb, blocks, blocks_per_slice)) < 0)
            return ret;
        if ((ret = decode_ac_coeffs(avctx, &gb, blocks, blocks_per_slice)) < 0)
            return ret;
    }

    block = blocks;
    for (i = 0; i < slice->mb_count; i++) {
        for (j = 0; j < log2_blocks_per_mb; j++) {
            ctx->prodsp.idct_put(dst,              dst_stride, block+(0<<6), qmat);
            ctx->prodsp.idct_put(dst+4*dst_stride, dst_stride, block+(1<<6), qmat);
            block += 2*64;
            dst += 8;
        }
    }
    return 0;
}

/**
 * Decode alpha slice plane.
 */
static void decode_slice_alpha(const ProresContext *ctx,
                               uint16_t *dst, int dst_stride,
                               const uint8_t *buf, int buf_size,
                               int blocks_per_slice)
{
    GetBitContext gb;
    int i;
    LOCAL_ALIGNED_32(int16_t, blocks, [8*4*64]);
    int16_t *block;

    for (i = 0; i < blocks_per_slice<<2; i++)
        ctx->bdsp.clear_block(blocks+(i<<6));

    init_get_bits(&gb, buf, buf_size << 3);

    if (ctx->alpha_info == 2) {
        ctx->unpack_alpha(&gb, blocks, blocks_per_slice * 4 * 64, 16);
    } else {
        ctx->unpack_alpha(&gb, blocks, blocks_per_slice * 4 * 64, 8);
    }

    block = blocks;

    for (i = 0; i < 16; i++) {
        memcpy(dst, block, 16 * blocks_per_slice * sizeof(*dst));
        dst   += dst_stride >> 1;
        block += 16 * blocks_per_slice;
    }
}

static int decode_slice_thread(AVCodecContext *avctx, void *arg, int jobnr, int threadnr)
{
    const ProresContext *ctx = avctx->priv_data;
    SliceContext *slice = &ctx->slices[jobnr];
    const uint8_t *buf = slice->data;
    AVFrame *pic = ctx->frame;
    int i, hdr_size, qscale, log2_chroma_blocks_per_mb;
    int luma_stride, chroma_stride;
    int y_data_size, u_data_size, v_data_size, a_data_size, offset;
    uint8_t *dest_y, *dest_u, *dest_v;
    LOCAL_ALIGNED_16(int16_t, qmat_luma_scaled,  [64]);
    LOCAL_ALIGNED_16(int16_t, qmat_chroma_scaled,[64]);
    int mb_x_shift;
    int ret;

    slice->ret = -1;
    //av_log(avctx, AV_LOG_INFO, "slice %d mb width %d mb x %d y %d\n",
    //       jobnr, slice->mb_count, slice->mb_x, slice->mb_y);

    // slice header
    hdr_size = buf[0] >> 3;
    qscale = av_clip(buf[1], 1, 224);
    qscale = qscale > 128 ? qscale - 96 << 2: qscale;
    y_data_size = AV_RB16(buf + 2);
    u_data_size = AV_RB16(buf + 4);
    v_data_size = slice->data_size - y_data_size - u_data_size - hdr_size;
    if (hdr_size > 7) v_data_size = AV_RB16(buf + 6);
    a_data_size = slice->data_size - y_data_size - u_data_size -
                  v_data_size - hdr_size;

    if (y_data_size < 0 || u_data_size < 0 || v_data_size < 0
        || hdr_size+y_data_size+u_data_size+v_data_size > slice->data_size){
        av_log(avctx, AV_LOG_ERROR, "invalid plane data size\n");
        return AVERROR_INVALIDDATA;
    }

    buf += hdr_size;

    for (i = 0; i < 64; i++) {
        qmat_luma_scaled  [i] = ctx->qmat_luma  [i] * qscale;
        qmat_chroma_scaled[i] = ctx->qmat_chroma[i] * qscale;
    }

    if (ctx->frame_type == 0) {
        luma_stride   = pic->linesize[0];
        chroma_stride = pic->linesize[1];
    } else {
        luma_stride   = pic->linesize[0] << 1;
        chroma_stride = pic->linesize[1] << 1;
    }

    if (avctx->pix_fmt == AV_PIX_FMT_YUV444P10 || avctx->pix_fmt == AV_PIX_FMT_YUVA444P10 ||
        avctx->pix_fmt == AV_PIX_FMT_YUV444P12 || avctx->pix_fmt == AV_PIX_FMT_YUVA444P12) {
        mb_x_shift = 5;
        log2_chroma_blocks_per_mb = 2;
    } else {
        mb_x_shift = 4;
        log2_chroma_blocks_per_mb = 1;
    }

    offset = (slice->mb_y << 4) * luma_stride + (slice->mb_x << 5);
    dest_y = pic->data[0] + offset;
    dest_u = pic->data[1] + (slice->mb_y << 4) * chroma_stride + (slice->mb_x << mb_x_shift);
    dest_v = pic->data[2] + (slice->mb_y << 4) * chroma_stride + (slice->mb_x << mb_x_shift);

    if (ctx->frame_type && ctx->first_field ^ !!(ctx->frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST)) {
        dest_y += pic->linesize[0];
        dest_u += pic->linesize[1];
        dest_v += pic->linesize[2];
        offset += pic->linesize[3];
    }

    ret = decode_slice_luma(avctx, slice, (uint16_t*)dest_y, luma_stride,
                            buf, y_data_size, qmat_luma_scaled);
    if (ret < 0)
        return ret;

    if (!(avctx->flags & AV_CODEC_FLAG_GRAY)) {
        ret = decode_slice_chroma(avctx, slice, (uint16_t*)dest_u, chroma_stride,
                                  buf + y_data_size, u_data_size,
                                  qmat_chroma_scaled, log2_chroma_blocks_per_mb);
        if (ret < 0)
            return ret;

        ret = decode_slice_chroma(avctx, slice, (uint16_t*)dest_v, chroma_stride,
                                  buf + y_data_size + u_data_size, v_data_size,
                                  qmat_chroma_scaled, log2_chroma_blocks_per_mb);
        if (ret < 0)
            return ret;
    }

    /* decode alpha plane if available */
    if (ctx->alpha_info && pic->data[3] && a_data_size) {
        uint8_t *dest_a = pic->data[3] + offset;
        decode_slice_alpha(ctx, (uint16_t*)dest_a, luma_stride,
                           buf + y_data_size + u_data_size + v_data_size,
                           a_data_size, slice->mb_count);
    }

    slice->ret = 0;
    return 0;
}

static int decode_picture(AVCodecContext *avctx)
{
    ProresContext *ctx = avctx->priv_data;
    int i;
    int error = 0;

    avctx->execute2(avctx, decode_slice_thread, NULL, NULL, ctx->slice_count);

    for (i = 0; i < ctx->slice_count; i++)
        error += ctx->slices[i].ret < 0;

    if (error)
        ctx->frame->decode_error_flags = FF_DECODE_ERROR_INVALID_BITSTREAM;
    if (error < ctx->slice_count)
        return 0;

    return ctx->slices[0].ret;
}

static int decode_frame(AVCodecContext *avctx, AVFrame *frame,
                        int *got_frame, AVPacket *avpkt)
{
    ProresContext *ctx = avctx->priv_data;
    const uint8_t *buf = avpkt->data;
    int buf_size = avpkt->size;
    int frame_hdr_size, pic_size, ret;

    if (buf_size < 28 || AV_RL32(buf + 4) != AV_RL32("icpf")) {
        av_log(avctx, AV_LOG_ERROR, "invalid frame header\n");
        return AVERROR_INVALIDDATA;
    }

    ctx->frame = frame;
    ctx->first_field = 1;

    buf += 8;
    buf_size -= 8;

    frame_hdr_size = decode_frame_header(ctx, buf, buf_size, avctx);
    if (frame_hdr_size < 0)
        return frame_hdr_size;

    buf += frame_hdr_size;
    buf_size -= frame_hdr_size;

    if ((ret = ff_thread_get_buffer(avctx, frame, 0)) < 0)
        return ret;
    ff_thread_finish_setup(avctx);

    if (HWACCEL_MAX && avctx->hwaccel) {
        const FFHWAccel *hwaccel = ffhwaccel(avctx->hwaccel);
        ret = hwaccel->start_frame(avctx, avpkt->buf, avpkt->data, avpkt->size);
        if (ret < 0)
            return ret;
        ret = hwaccel->decode_slice(avctx, avpkt->data, avpkt->size);
        if (ret < 0)
            return ret;
        ret = hwaccel->end_frame(avctx);
        if (ret < 0)
            return ret;
        goto finish;
    }

 decode_picture:
    pic_size = decode_picture_header(avctx, buf, buf_size);
    if (pic_size < 0) {
        av_log(avctx, AV_LOG_ERROR, "error decoding picture header\n");
        return pic_size;
    }

    if ((ret = decode_picture(avctx)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "error decoding picture\n");
        return ret;
    }

    buf += pic_size;
    buf_size -= pic_size;

    if (ctx->frame_type && buf_size > 0 && ctx->first_field) {
        ctx->first_field = 0;
        goto decode_picture;
    }

finish:
    *got_frame      = 1;

    return avpkt->size;
}

static av_cold int decode_close(AVCodecContext *avctx)
{
    ProresContext *ctx = avctx->priv_data;

    av_freep(&ctx->slices);

    return 0;
}

#if HAVE_THREADS
static int update_thread_context(AVCodecContext *dst, const AVCodecContext *src)
{
    ProresContext *csrc = src->priv_data;
    ProresContext *cdst = dst->priv_data;

    cdst->pix_fmt = csrc->pix_fmt;

    return 0;
}
#endif

const FFCodec ff_prores_decoder = {
    .p.name         = "prores",
    CODEC_LONG_NAME("Apple ProRes (iCodec Pro)"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_PRORES,
    .priv_data_size = sizeof(ProresContext),
    .init           = decode_init,
    .close          = decode_close,
    FF_CODEC_DECODE_CB(decode_frame),
    UPDATE_THREAD_CONTEXT(update_thread_context),
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_SLICE_THREADS | AV_CODEC_CAP_FRAME_THREADS,
    .p.profiles     = NULL_IF_CONFIG_SMALL(ff_prores_profiles),
#if HWACCEL_MAX
    .hw_configs     = (const AVCodecHWConfigInternal *const []) {
#if CONFIG_PRORES_VIDEOTOOLBOX_HWACCEL
        HWACCEL_VIDEOTOOLBOX(prores),
#endif
        NULL
    },
#endif
};
