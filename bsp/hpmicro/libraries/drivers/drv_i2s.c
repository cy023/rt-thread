/*
 * Copyright (c) 2022 hpmicro
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include <rtthread.h>
#include <rtdevice.h>

#define DBG_TAG              "i2s"
#define DBG_LVL              DBG_INFO
#include <rtdbg.h>

#ifdef BSP_USING_I2S
#include "hpm_i2s_drv.h"
#include "board.h"
#include "hpm_dma_drv.h"
#include "hpm_dmamux_drv.h"
#include "hpm_l1c_drv.h"
#include "hpm_clock_drv.h"
#include "hpm_dma_manager.h"

#include "drv_i2s.h"
#include "drivers/audio.h"

static rt_ssize_t hpm_i2s_transmit(struct rt_audio_device* audio, const void* writeBuf, void* readBuf, rt_size_t size);

struct hpm_i2s
{
    struct rt_audio_device audio;
    struct rt_audio_configure audio_config;
    hpm_dma_resource_t rx_dma_resource;
    hpm_dma_resource_t tx_dma_resource;
    char *dev_name;
    I2S_Type *base;
    clock_name_t clk_name;
    i2s_transfer_config_t transfer;
    uint8_t rx_dma_req;
    uint8_t tx_dma_req;
    rt_uint8_t* tx_buff;
    rt_uint8_t* rx_buff;
};

#if defined(BSP_USING_I2S0)
ATTR_ALIGN(HPM_L1C_CACHELINE_SIZE) uint8_t i2s0_tx_buff[I2S_FIFO_SIZE];
ATTR_ALIGN(HPM_L1C_CACHELINE_SIZE) uint8_t i2s0_rx_buff[I2S_FIFO_SIZE];
#endif
#if defined(BSP_USING_I2S1)
ATTR_ALIGN(HPM_L1C_CACHELINE_SIZE) uint8_t i2s1_tx_buff[I2S_FIFO_SIZE];
ATTR_ALIGN(HPM_L1C_CACHELINE_SIZE) uint8_t i2s1_rx_buff[I2S_FIFO_SIZE];
#endif
#if defined(BSP_USING_I2S2)
ATTR_ALIGN(HPM_L1C_CACHELINE_SIZE) uint8_t i2s2_tx_buff[I2S_FIFO_SIZE];
ATTR_ALIGN(HPM_L1C_CACHELINE_SIZE) uint8_t i2s2_rx_buff[I2S_FIFO_SIZE];
#endif
#if defined(BSP_USING_I2S3)
ATTR_ALIGN(HPM_L1C_CACHELINE_SIZE) uint8_t i2s3_tx_buff[I2S_FIFO_SIZE];
ATTR_ALIGN(HPM_L1C_CACHELINE_SIZE) uint8_t i2s3_rx_buff[I2S_FIFO_SIZE];
#endif

static struct hpm_i2s hpm_i2s_set[] =
{
#if defined(BSP_USING_I2S0)
    {
        .dev_name = "i2s0",
        .base = HPM_I2S0,
        .clk_name =  clock_i2s0,
        .rx_dma_req = HPM_DMA_SRC_I2S0_RX,
        .tx_dma_req = HPM_DMA_SRC_I2S0_TX,
        .tx_buff = i2s0_tx_buff,
        .rx_buff = i2s0_rx_buff,
    },
#endif
#if defined(BSP_USING_I2S1)
    {
        .dev_name = "i2s1",
        .base = HPM_I2S1;
        .clk_name =  clock_i2s1,
        .rx_dma_req = HPM_DMA_SRC_I2S1_RX,
        .tx_dma_req = HPM_DMA_SRC_I2S1_TX,
        .tx_buff = i2s1_tx_buff,
        .rx_buff = i2s1_rx_buff,
    },
#endif
#if defined(BSP_USING_I2S2)
    {
        .dev_name = "i2s2",
        .base = HPM_I2S2,
        .clk_name =  clock_i2s2,
        .rx_dma_req = HPM_DMA_SRC_I2S2_RX,
        .tx_dma_req = HPM_DMA_SRC_I2S2_TX,
        .tx_buff = i2s2_tx_buff,
        .rx_buff = i2s2_rx_buff,
    },
#endif
#if defined(BSP_USING_I2S3)
    {
        .dev_name = "i2s3",
        .base = HPM_I2S3,
        .clk_name =  clock_i2s3,
        .rx_dma_req = HPM_DMA_SRC_I2S3_RX,
        .tx_dma_req = HPM_DMA_SRC_I2S3_TX,
        .tx_buff = i2s3_tx_buff,
        .rx_buff = i2s3_rx_buff,
    },
#endif
};

/* I2S TX DMA callback function: trigger next transfer */
void i2s_tx_dma_callback(DMA_Type *ptr, uint32_t channel, void *user_data, uint32_t int_stat)
{
    if (int_stat == DMA_CHANNEL_STATUS_TC) {
        struct hpm_i2s* hpm_audio = (struct hpm_i2s*) user_data;
        rt_audio_tx_complete(&hpm_audio->audio);
    }
}

/* I2S RX DMA callback function: write data into record->pipe and trigger next transfer */
void i2s_rx_dma_callback(DMA_Type *ptr, uint32_t channel, void *user_data, uint32_t int_stat)
{
    if (int_stat == DMA_CHANNEL_STATUS_TC) {
        struct hpm_i2s* hpm_audio = (struct hpm_i2s*) user_data;
        rt_audio_rx_done(&hpm_audio->audio, hpm_audio->rx_buff, I2S_FIFO_SIZE);
        hpm_i2s_transmit(&hpm_audio->audio, NULL, hpm_audio->rx_buff, I2S_FIFO_SIZE);
    }
}


static rt_err_t hpm_i2s_init(struct rt_audio_device* audio)
{
    RT_ASSERT(audio != RT_NULL);
    rt_uint32_t mclk_hz;
    i2s_config_t i2s_config;
    i2s_transfer_config_t transfer;

    struct hpm_i2s* hpm_audio = (struct hpm_i2s*)audio->parent.user_data;

    init_i2s_pins(hpm_audio->base);
    board_init_i2s_clock(hpm_audio->base);

    //使用DMA传输
    i2s_enable_rx_dma_request(hpm_audio->base);
    i2s_enable_tx_dma_request(hpm_audio->base);

    i2s_get_default_config(hpm_audio->base, &i2s_config);
    i2s_config.enable_mclk_out = true;
    i2s_config.frame_start_at_rising_edge = true;  //左对齐与右对齐方式， 对应上升沿
    i2s_init(hpm_audio->base, &i2s_config);

    mclk_hz = clock_get_frequency(hpm_audio->clk_name);
    i2s_get_default_transfer_config(&transfer);
    /* 初始化I2S配置, 应用使用configure ops修改属性  */
    transfer.sample_rate = 24000U;
    transfer.protocol = I2S_PROTOCOL_LEFT_JUSTIFIED;
    transfer.channel_slot_mask = I2S_CHANNEL_SLOT_MASK(0); /* 1个通道 */
    transfer.audio_depth = I2S_AUDIO_DEPTH_16_BITS;
    transfer.master_mode = true;
    hpm_audio->transfer = transfer;
    //将初始参数记录到audio_config
    hpm_audio->audio_config.samplerate = 24000U;
    hpm_audio->audio_config.samplebits = 16;
    hpm_audio->audio_config.channels = 1;
    if (status_success != i2s_config_transfer(hpm_audio->base, mclk_hz, &transfer))
    {
        LOG_E("dao_i2s configure transfer failed\n");
        return -RT_ERROR;
    }

    return RT_EOK;
}

static rt_err_t hpm_i2s_getcaps(struct rt_audio_device* audio, struct rt_audio_caps* caps)
{
    rt_err_t result = RT_EOK;

    RT_ASSERT(audio != RT_NULL);
    struct hpm_i2s* hpm_audio = (struct hpm_i2s*)audio->parent.user_data;

    switch(caps->main_type)
    {
        case AUDIO_TYPE_INPUT:
        {
            switch(caps->sub_type)
            {
                case AUDIO_DSP_PARAM:
                {
                    caps->udata.config.channels     = hpm_audio->audio_config.channels;
                    caps->udata.config.samplebits   = hpm_audio->audio_config.samplebits;
                    caps->udata.config.samplerate   = hpm_audio->audio_config.samplerate;
                    break;
                }

                case AUDIO_DSP_SAMPLERATE:
                {
                    caps->udata.config.samplerate   = hpm_audio->audio_config.samplerate;
                    break;
                }

                case AUDIO_DSP_CHANNELS:
                {
                    caps->udata.config.channels     = hpm_audio->audio_config.channels;
                    break;
                }

                case AUDIO_DSP_SAMPLEBITS:
                {
                    caps->udata.config.samplebits   = hpm_audio->audio_config.samplebits;
                    break;
                }

                case AUDIO_PARM_I2S_DATA_LINE:
                {
                    caps->udata.value               = hpm_audio->transfer.data_line;
                    break;
                }

                default:
                {
                    result = -RT_ERROR;
                    break;
                }
            }
            break;
        }
        case AUDIO_TYPE_OUTPUT:
        {
            switch(caps->sub_type)
            {
                case AUDIO_DSP_PARAM:
                {
                    caps->udata.config.samplerate   = hpm_audio->audio_config.samplerate;
                    caps->udata.config.channels     = hpm_audio->audio_config.channels;
                    caps->udata.config.samplebits   = hpm_audio->audio_config.samplebits;
                    break;
                }

                case AUDIO_DSP_SAMPLERATE:
                {
                    caps->udata.config.samplerate   = hpm_audio->audio_config.samplerate;
                    break;
                }

                case AUDIO_DSP_CHANNELS:
                {
                    caps->udata.config.channels     = hpm_audio->audio_config.channels;
                    break;
                }

                case AUDIO_DSP_SAMPLEBITS:
                {
                    caps->udata.config.samplebits   = hpm_audio->audio_config.samplebits;
                    break;
                }

                case AUDIO_PARM_I2S_DATA_LINE:
                {
                    caps->udata.value               = hpm_audio->transfer.data_line;
                    break;
                }

                default:
                {
                    result = -RT_ERROR;
                    break;
                }
            }

            break;
        }

        default:
            result = -RT_ERROR;
            break;
    }

    return result;
}

static rt_err_t hpm_i2s_configure(struct rt_audio_device* audio, struct rt_audio_caps* caps)
{

    rt_err_t result = RT_EOK;
    RT_ASSERT(audio != RT_NULL);
    struct hpm_i2s* hpm_audio = (struct hpm_i2s*)audio->parent.user_data;

    switch(caps->main_type)
    {
        case AUDIO_TYPE_OUTPUT:
        {
            switch(caps->sub_type)
            {
            case AUDIO_DSP_PARAM:
            {
                hpm_audio->audio_config.samplerate = caps->udata.config.samplerate;
                hpm_audio->audio_config.samplebits = caps->udata.config.samplebits;
                hpm_audio->audio_config.channels = caps->udata.config.channels;
                break;
            }

            case AUDIO_DSP_SAMPLERATE:
            {
                hpm_audio->audio_config.samplerate = caps->udata.config.samplerate;
                break;
            }

            case AUDIO_DSP_CHANNELS:
            {
                hpm_audio->audio_config.channels = caps->udata.config.channels;
                break;
            }

            case AUDIO_DSP_SAMPLEBITS:
            {
                hpm_audio->audio_config.samplebits = caps->udata.config.samplebits;
                break;
            }

            case AUDIO_PARM_I2S_DATA_LINE:
            {
                hpm_audio->transfer.data_line      = caps->udata.value;
                break;
            }

            default:
                result = -RT_ERROR;
                break;
            }
            break;
        }
        case AUDIO_TYPE_INPUT:
        {
            switch(caps->sub_type)
            {

            case AUDIO_DSP_PARAM:
            {
                hpm_audio->audio_config.samplerate = caps->udata.config.samplerate;
                hpm_audio->audio_config.channels   = caps->udata.config.channels;
                hpm_audio->audio_config.samplebits = caps->udata.config.samplebits;
                break;
            }

            case AUDIO_DSP_SAMPLERATE:
            {
                hpm_audio->audio_config.samplerate = caps->udata.config.samplerate;
                break;
            }
            case AUDIO_DSP_CHANNELS:
            {
                hpm_audio->audio_config.channels = caps->udata.config.channels;
                break;
            }

            case AUDIO_DSP_SAMPLEBITS:
            {
                hpm_audio->audio_config.samplebits = caps->udata.config.samplebits;
                break;
            }

            case AUDIO_PARM_I2S_DATA_LINE:
            {
                hpm_audio->transfer.data_line      = caps->udata.value;
                break;
            }

            default:
                result = -RT_ERROR;
                break;
            }
            break;
        }

        default:
            break;
    }

    /* 设置 I2S transfer */
    if (hpm_audio->audio_config.channels == i2s_mono_left) {
        hpm_audio->transfer.channel_slot_mask = I2S_CHANNEL_SLOT_MASK(0);
    } else if (hpm_audio->audio_config.channels == i2s_mono_right) {
        hpm_audio->transfer.channel_slot_mask = I2S_CHANNEL_SLOT_MASK(1);
    } else if(hpm_audio->audio_config.channels == 2) {
        hpm_audio->transfer.channel_slot_mask = I2S_CHANNEL_SLOT_MASK(0) | I2S_CHANNEL_SLOT_MASK(1);
    } else {
        LOG_E("I2S not support channels number %d.\n", hpm_audio->audio_config.channels);
        return -RT_ERROR;
    }

    hpm_audio->transfer.sample_rate = hpm_audio->audio_config.samplerate;

    //i2s dma方式仅支持采样位宽为：16bit, 32bit
    assert(hpm_audio->audio_config.samplebits == 16 || hpm_audio->audio_config.samplebits == 32);
    hpm_audio->transfer.audio_depth = (hpm_audio->audio_config.samplebits - 16) >> 3;

    if (status_success != i2s_config_transfer(hpm_audio->base, clock_get_frequency(hpm_audio->clk_name), &hpm_audio->transfer))
    {
        LOG_E("%s configure transfer failed.\n", hpm_audio->dev_name);
    }
    return result;
}

static rt_err_t hpm_i2s_start(struct rt_audio_device* audio, int stream)
{
    RT_ASSERT(audio != RT_NULL);

    struct hpm_i2s* hpm_audio = (struct hpm_i2s*)audio->parent.user_data;

    /* 申请DMA resource用于I2S transfer */
    if (stream == AUDIO_STREAM_REPLAY) {
        hpm_dma_resource_t *dma_resource = &hpm_audio->tx_dma_resource;
        if (dma_manager_request_resource(dma_resource) == status_success) {
            uint8_t dmamux_ch;
            dma_manager_install_interrupt_callback(dma_resource, i2s_tx_dma_callback, hpm_audio);
            dma_manager_enable_dma_interrupt(dma_resource, 1);
            dmamux_ch = DMA_SOC_CHN_TO_DMAMUX_CHN(dma_resource->base, dma_resource->channel);
            dmamux_config(HPM_DMAMUX, dmamux_ch, hpm_audio->tx_dma_req, true);
        } else {
            LOG_E("no dma resource available for I2S TX transfer.\n");
            return -RT_ERROR;
        }
        rt_audio_tx_complete(audio);
    } else if (stream == AUDIO_STREAM_RECORD) {
        hpm_dma_resource_t *dma_resource = &hpm_audio->rx_dma_resource;
        if (dma_manager_request_resource(dma_resource) == status_success) {
            uint8_t dmamux_ch;
            dma_manager_install_interrupt_callback(dma_resource, i2s_rx_dma_callback, hpm_audio);
            dma_manager_enable_dma_interrupt(dma_resource, 1);
            dmamux_ch = DMA_SOC_CHN_TO_DMAMUX_CHN(dma_resource->base, dma_resource->channel);
            dmamux_config(HPM_DMAMUX, dmamux_ch, hpm_audio->rx_dma_req, true);
        } else {
            LOG_E("no dma resource available for I2S RX transfer.\n");
            return -RT_ERROR;
        }

        if (RT_EOK != hpm_i2s_transmit(&hpm_audio->audio, NULL, hpm_audio->rx_buff, I2S_FIFO_SIZE)) {
            return -RT_ERROR;
        }
    } else {
        return -RT_ERROR;
    }

    return RT_EOK;
}

static rt_err_t hpm_i2s_stop(struct rt_audio_device* audio, int stream)
{
    RT_ASSERT(audio != RT_NULL);
    struct hpm_i2s* hpm_audio = (struct hpm_i2s*)audio->parent.user_data;

    if (stream == AUDIO_STREAM_REPLAY) {
        hpm_dma_resource_t *dma_resource = &hpm_audio->tx_dma_resource;
        dma_manager_release_resource(dma_resource);
    } else if (stream == AUDIO_STREAM_RECORD)
    {
        hpm_dma_resource_t *dma_resource = &hpm_audio->rx_dma_resource;
        dma_manager_release_resource(dma_resource);
    } else {
        return -RT_ERROR;
    }

    return RT_EOK;
}

static rt_ssize_t hpm_i2s_transmit(struct rt_audio_device* audio, const void* writeBuf, void* readBuf, rt_size_t size)
{
    RT_ASSERT(audio != RT_NULL);
    struct hpm_i2s* hpm_audio = (struct hpm_i2s*)audio->parent.user_data;

    //支持采样位宽16bit, 32bit
    uint8_t data_width;
    uint8_t data_shift_byte;
    if (hpm_audio->transfer.audio_depth == I2S_AUDIO_DEPTH_16_BITS) {
        data_width = DMA_TRANSFER_WIDTH_HALF_WORD;
        data_shift_byte = 2U ; //16位音频数据位于寄存器的高位
    } else {
        data_width = DMA_TRANSFER_WIDTH_WORD;
        data_shift_byte = 0U;
    }

    if(writeBuf != RT_NULL)
    {
        hpm_dma_resource_t *dma_resource = &hpm_audio->tx_dma_resource;
        dma_channel_config_t ch_config = {0};
        dma_default_channel_config(dma_resource->base, &ch_config);
        ch_config.src_addr = core_local_mem_to_sys_address(HPM_CORE0, (uint32_t)writeBuf);
        ch_config.dst_addr = (uint32_t)&hpm_audio->base->TXD[hpm_audio->transfer.data_line] + data_shift_byte;
        ch_config.src_width = data_width;
        ch_config.dst_width = data_width;
        ch_config.src_addr_ctrl = DMA_ADDRESS_CONTROL_INCREMENT;
        ch_config.dst_addr_ctrl = DMA_ADDRESS_CONTROL_FIXED;
        ch_config.size_in_byte = size;
        ch_config.dst_mode = DMA_HANDSHAKE_MODE_HANDSHAKE;
        ch_config.src_burst_size = DMA_NUM_TRANSFER_PER_BURST_1T;

        if (l1c_dc_is_enabled()) {
            /* cache writeback for sent buff */
            l1c_dc_writeback((uint32_t)writeBuf, size);
        }

        if (status_success != dma_setup_channel(dma_resource->base, dma_resource->channel, &ch_config)) {
            LOG_E("dma setup channel failed\n");
            return -RT_ERROR;
        }
    } else if (readBuf != RT_NULL){
        hpm_dma_resource_t *dma_resource = &hpm_audio->rx_dma_resource;
        dma_channel_config_t ch_config = {0};
        dma_default_channel_config(dma_resource->base, &ch_config);
        ch_config.src_addr = (uint32_t)&hpm_audio->base->RXD[hpm_audio->transfer.data_line] + data_shift_byte;
        ch_config.dst_addr = core_local_mem_to_sys_address(HPM_CORE0, (uint32_t)readBuf);
        ch_config.src_width = data_width;
        ch_config.dst_width = data_width;
        ch_config.src_addr_ctrl = DMA_ADDRESS_CONTROL_FIXED;
        ch_config.dst_addr_ctrl = DMA_ADDRESS_CONTROL_INCREMENT;
        ch_config.size_in_byte = size;
        ch_config.src_mode = DMA_HANDSHAKE_MODE_HANDSHAKE;
        ch_config.src_burst_size = DMA_NUM_TRANSFER_PER_BURST_1T;

        if (status_success != dma_setup_channel(dma_resource->base, dma_resource->channel, &ch_config)) {
            LOG_E("dma setup channel failed\n");
            return -RT_ERROR;
        }

        if (l1c_dc_is_enabled()) {
            /* cache invalidate for receive buff */
            l1c_dc_invalidate((uint32_t)readBuf, size);
        }
    }

    return size;
}

static void hpm_i2s_buffer_info(struct rt_audio_device* audio, struct rt_audio_buf_info* info)
{
    RT_ASSERT(audio != RT_NULL);
    struct hpm_i2s* hpm_audio = (struct hpm_i2s*)audio->parent.user_data;
    /**
     *               AUD_FIFO
     * +----------------+----------------+
     * |     block1     |     block2     |
     * +----------------+----------------+
     *  \  block_size  /
     */
    info->buffer      = hpm_audio->tx_buff;
    info->total_size  = I2S_FIFO_SIZE;
    info->block_size  = I2S_FIFO_SIZE / 2;
    info->block_count = 2;
}


static struct rt_audio_ops hpm_i2s_ops =
{
    .getcaps     = hpm_i2s_getcaps,
    .configure   = hpm_i2s_configure,
    .init        = hpm_i2s_init,
    .start       = hpm_i2s_start,
    .stop        = hpm_i2s_stop,
    .transmit    = hpm_i2s_transmit,
    .buffer_info = hpm_i2s_buffer_info,
};

int rt_hw_i2s_init(void)
{
    rt_err_t ret = RT_EOK;

    for (uint32_t i = 0; i < sizeof(hpm_i2s_set) / sizeof(hpm_i2s_set[0]); i++) {
        hpm_i2s_set[i].audio.ops = &hpm_i2s_ops;

        ret = rt_audio_register(&hpm_i2s_set[i].audio, hpm_i2s_set[i].dev_name, RT_DEVICE_FLAG_RDWR, &hpm_i2s_set[i]);

        if (ret != RT_EOK)
        {
            LOG_E("rt audio %s register failed, status=%d\n", hpm_i2s_set[i].dev_name, ret);
        }

    }

    return RT_EOK;
}
INIT_DEVICE_EXPORT(rt_hw_i2s_init);


#endif /* BSP_USING_I2S */

