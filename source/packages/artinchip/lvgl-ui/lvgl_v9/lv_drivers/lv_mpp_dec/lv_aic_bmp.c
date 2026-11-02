/*
 * Copyright (C) 2024-2025, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:  Ning Fang <ning.fang@artinchip.com>
 */

#include <stdlib.h>
#include <string.h>

#include "aic_core.h"
#include "lv_aic_stream.h"
#include "lv_mpp_dec.h"
#include "lv_aic_bmp.h"

#if LV_USE_AIC_BMP

lv_result_t lv_check_bmp_header(lv_stream_t *stream)
{
    uint32_t read_num;
    uint8_t buf[128];
    lv_fs_res_t fs_res;
    lv_result_t res = LV_RESULT_OK;

    fs_res = lv_aic_stream_read(stream, buf, 2, &read_num);
    if (fs_res != LV_FS_RES_OK || read_num != 2) {
        res = LV_RESULT_INVALID;
        goto read_err;
    }

    if (buf[0] != 0x42 || buf[1] != 0x4d) {
        res = LV_RESULT_INVALID;
        goto read_err;
    }

read_err:
    return res;
}

lv_result_t lv_bmp_decoder_info(const char *src, lv_image_header_t *header, uint32_t size, bool is_file)
{
    lv_fs_res_t res = LV_FS_RES_OK;
    uint32_t read_num = 0;
    uint8_t buf[64] = { 0 };
    lv_stream_t stream= { 0 };
    int32_t w = 0;
    int32_t h = 0;
    uint16_t bpp = 0;

    if (is_file)
        res = lv_aic_stream_open_file(&stream, src);
    else
       res = lv_aic_stream_open_buf(&stream, src, size);

    if (res != LV_FS_RES_OK) {
        return LV_RESULT_INVALID;
    }

    res = lv_aic_stream_read(&stream, buf, 54, &read_num);
    if (res != LV_FS_RES_OK || read_num != 54) {
        LV_LOG_ERROR("lv_aic_stream_read failed");
        lv_aic_stream_close(&stream);
        return LV_RESULT_INVALID;
    }

    lv_memcpy(&w, buf + 18, 4);
    lv_memcpy(&h, buf + 22, 4);
    lv_memcpy(&bpp, buf + 28, 2);

    header->w = w;
    header->h = LV_ABS(h);
    header->stride = ((bpp * w + 31) / 32) * 4;
    header->stride = ALIGN_UP((int32_t)(header->stride), 8);

    switch(bpp) {
        case 16:
            header->cf = LV_COLOR_FORMAT_RGB565;
            break;
        case 24:
            header->cf = LV_COLOR_FORMAT_RGB888;
            break;
        case 32:
            header->cf = LV_COLOR_FORMAT_ARGB8888;
            break;
        default:
            LV_LOG_ERROR("Not supported bpp: %d", (int)bpp);
            lv_aic_stream_close(&stream);
            return LV_RESULT_INVALID;
    }

    LV_LOG_INFO("header->w:%d, header->h:%d, header->cf:%d", header->w, header->h, header->cf);
    lv_aic_stream_close(&stream);

    return LV_RESULT_OK;
}

#define LV_AIC_BMP_READ_CHUNK_SIZE (32U * 1024U)

/* 批量读取 BMP 行数据，减少 SD 卡文件系统逐行调用开销。 */
static lv_result_t lv_bmp_read_pixels(lv_stream_t *stream, uint8_t *dst,
                                      int32_t height, int32_t stride,
                                      int32_t aligned_stride, uint32_t image_size,
                                      bool bottom_up)
{
    uint32_t row_count = (uint32_t)height;
    uint32_t source_stride = (uint32_t)stride;
    uint32_t target_stride = (uint32_t)aligned_stride;
    uint32_t source_size = source_stride * row_count;
    uint32_t read_num = 0;
    uint8_t *scratch = NULL;
    lv_fs_res_t fs_res;

    if (!stream || !dst || height <= 0 || stride <= 0 || aligned_stride < stride)
        return LV_RESULT_INVALID;

    /* 常见 1024 像素宽 BMP 行已经对齐：一次顺序读取后原位翻转行序。 */
    if (source_stride == target_stride) {
        if (bottom_up) {
            scratch = (uint8_t *)lv_malloc(source_stride);
            if (!scratch)
                goto read_rows_fallback;
        }

        fs_res = lv_aic_stream_read(stream, dst, source_size, &read_num);
        if (fs_res != LV_FS_RES_OK || read_num != source_size) {
            if (scratch)
                lv_free(scratch);
            return LV_RESULT_INVALID;
        }

        if (bottom_up) {
            uint32_t top;
            for (top = 0; top < row_count / 2U; top++) {
                uint8_t *top_row = dst + (size_t)top * target_stride;
                uint8_t *bottom_row = dst + (size_t)(row_count - 1U - top) * target_stride;
                memcpy(scratch, top_row, source_stride);
                memcpy(top_row, bottom_row, source_stride);
                memcpy(bottom_row, scratch, source_stride);
            }
            lv_free(scratch);
        }
        if (image_size > source_size)
            memset(dst + source_size, 0, image_size - source_size);
        return LV_RESULT_OK;
    }

    /* 行尾需要补齐时，用不超过 32 KiB 的临时块批量读入并放置到目标行。 */
    {
        uint32_t rows_per_chunk = LV_AIC_BMP_READ_CHUNK_SIZE / source_stride;
        uint32_t source_row = 0;

        if (!rows_per_chunk)
            rows_per_chunk = 1U;
        scratch = (uint8_t *)lv_malloc((size_t)rows_per_chunk * source_stride);
        if (!scratch)
            goto read_rows_fallback;

        while (source_row < row_count) {
            uint32_t rows = row_count - source_row;
            uint32_t bytes;
            uint32_t row;

            if (rows > rows_per_chunk)
                rows = rows_per_chunk;
            bytes = rows * source_stride;
            read_num = 0;
            fs_res = lv_aic_stream_read(stream, scratch, bytes, &read_num);
            if (fs_res != LV_FS_RES_OK || read_num != bytes) {
                lv_free(scratch);
                return LV_RESULT_INVALID;
            }

            for (row = 0; row < rows; row++) {
                uint32_t source_index = source_row + row;
                uint32_t target_index = bottom_up ?
                    row_count - 1U - source_index : source_index;
                uint8_t *target = dst + (size_t)target_index * target_stride;
                memcpy(target, scratch + (size_t)row * source_stride, source_stride);
                memset(target + source_stride, 0, target_stride - source_stride);
            }
            source_row += rows;
        }

        lv_free(scratch);
    }

    if (image_size > target_stride * row_count)
        memset(dst + (size_t)target_stride * row_count, 0,
               image_size - target_stride * row_count);
    return LV_RESULT_OK;

read_rows_fallback:
    /* 内存紧张时保留原有逐行读取路径，保证图片仍可打开。 */
    if (scratch)
        lv_free(scratch);
    {
        uint32_t source_row;
        for (source_row = 0; source_row < row_count; source_row++) {
            uint32_t target_index = bottom_up ?
                row_count - 1U - source_row : source_row;
            uint8_t *target = dst + (size_t)target_index * target_stride;

            read_num = 0;
            fs_res = lv_aic_stream_read(stream, target, source_stride, &read_num);
            if (fs_res != LV_FS_RES_OK || read_num != source_stride)
                return LV_RESULT_INVALID;
            memset(target + source_stride, 0, target_stride - source_stride);
        }
    }
    if (image_size > target_stride * row_count)
        memset(dst + (size_t)target_stride * row_count, 0,
               image_size - target_stride * row_count);
    return LV_RESULT_OK;
}

lv_result_t lv_bmp_dec_open(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc)
{
    lv_result_t res = LV_RESULT_OK;
    lv_stream_t stream;
    uint8_t headers[64];
    uint32_t read_num = 0;
    lv_fs_res_t fs_res;
    int32_t width;
    int32_t height;
    int32_t abs_height;
    uint16_t bpp;
    int32_t stride;
    int32_t stride_aligned;
    int32_t image_size;
    unsigned int align_addr;

    if (lv_aic_stream_open(&stream, dsc->src) != LV_FS_RES_OK)
        return LV_RESULT_INVALID;

    mpp_decoder_data_t *mpp_data = (mpp_decoder_data_t *)lv_malloc_zeroed(sizeof(mpp_decoder_data_t));
    CHECK_PTR(mpp_data);

    fs_res = lv_aic_stream_read(&stream, headers, 54, &read_num);
    if (fs_res != LV_FS_RES_OK || read_num != 54) {
        LV_LOG_ERROR("lv_aic_stream_read failed");
        res = LV_RESULT_INVALID;
        goto out;
    }

    lv_memcpy(&width, headers + 18, 4);
    lv_memcpy(&height, headers + 22, 4);
    lv_memcpy(&bpp, headers + 28, 2);

    stride = ((bpp * width + 31) / 32) * 4;
    stride_aligned = ALIGN_UP(stride, 8);
    abs_height = LV_ABS(height);
    image_size = ALIGN_UP(abs_height * stride_aligned, CACHE_LINE_SIZE);

    mpp_data->data[0] = (uint8_t *)aicos_malloc_try_cma(image_size + CACHE_LINE_SIZE - 1);
    if (!mpp_data->data[0]) {
        res = LV_RESULT_INVALID;
        goto out;
    } else {
        align_addr = (unsigned int)ALIGN_UP((ulong)mpp_data->data[0], CACHE_LINE_SIZE);
    }

    mpp_data->decoded.header.w = width;
    mpp_data->decoded.header.h = abs_height;
    mpp_data->decoded.header.cf = dsc->header.cf;
    mpp_data->decoded.header.magic = LV_IMAGE_HEADER_MAGIC;
    mpp_data->decoded.header.stride = stride_aligned;
    mpp_data->decoded.data =(uint8_t *)((ulong)align_addr);
    mpp_data->decoded.data_size = image_size;
    mpp_data->decoded.unaligned_data = mpp_data->data[0];

    res = lv_bmp_read_pixels(&stream, mpp_data->decoded.data, abs_height,
                             stride, stride_aligned, (uint32_t)image_size,
                             height > 0);
    if (res != LV_RESULT_OK)
        goto out;
    aicos_dcache_clean_invalid_range((ulong *)(ulong)align_addr, ALIGN_UP(image_size, CACHE_LINE_SIZE));

    dsc->decoded = &mpp_data->decoded;
#if LV_CACHE_DEF_SIZE > 0
    if (!dsc->args.no_cache) {
        mpp_cache_t *mpp_cache = (mpp_cache_t *)decoder->user_data;
        if (lv_image_cache_check(mpp_cache)) {
            // Add it to cache
            lv_image_cache_data_t search_key;
            search_key.src_type = dsc->src_type;
            search_key.src = dsc->src;
            search_key.slot.size = dsc->decoded->data_size;
            mpp_data->cached = true;
            lv_cache_entry_t *cache_entry = lv_image_decoder_add_to_cache(decoder, &search_key, dsc->decoded, mpp_data);
            CHECK_PTR(cache_entry);
            dsc->cache_entry = cache_entry;
            lv_mutex_lock(&mpp_cache->lock);
            mpp_cache->cache_num++;
            lv_mutex_unlock(&mpp_cache->lock);
        }
    }
#endif

out:
    if (res == LV_RESULT_INVALID) {
        dsc->decoded = NULL;
        if (mpp_data) {
            lv_frame_buf_free(mpp_data);
            lv_free(mpp_data);
        }
    }

    lv_aic_stream_close(&stream);
    return res;
}

#endif
