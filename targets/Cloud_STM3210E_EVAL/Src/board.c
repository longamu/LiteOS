/*----------------------------------------------------------------------------
 * Copyright (c) <2016-2018>, <Huawei Technologies Co., Ltd>
 * All rights reserved.
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice, this list of
 * conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list
 * of conditions and the following disclaimer in the documentation and/or other materials
 * provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used
 * to endorse or promote products derived from this software without specific prior written
 * permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *---------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------
 * Notice of Export Control Law
 * ===============================================
 * Huawei LiteOS may be subject to applicable export control laws and regulations, which might
 * include those applicable to Huawei LiteOS of U.S. and the country in which you are located.
 * Import, export and usage of Huawei LiteOS in any manner by you shall be in compliance with such
 * applicable export control laws and regulations.
 *---------------------------------------------------------------------------*/

#include "stm32f1xx.h"
#include "board.h"
#include "ota.h"
#include "hal_flash.h"
#include "hal_spi_flash.h"

#define OTA_FLASH_BASE             0x08000000
#define OTA_MEMORY_BASE            0x20000000

#define OTA_PC_MASK                0xFF000000
#define OTA_STACK_MASK             0x2FF00000

#define OTA_COPY_BUF_SIZE          0x1000
#define OTA_INNER_FLASH_BLOCK_SIZE 0x20000
#define OTA_RECORD_OFFSET_SIZE     0x4000

typedef enum
{
    BOARD_INIT = 0,
    BOARD_BCK,
    BOARD_UPDATE,
    BOARD_ROLLBACK,
} board_state;

typedef void (*jump_func)(void);

static void set_msp(uint32_t stack)
{
    __set_MSP(stack);
}

static int prv_spi2inner_copy(uint32_t addr_source,
                              int32_t image_len,
                              board_state state,
                              uint32_t offset,
                              int (*func_set_update_record)(uint8_t state, uint32_t offset))
{
    int ret;
    int32_t copy_len;
    uint8_t buf[OTA_COPY_BUF_SIZE];
    uint32_t addr_dest = OTA_DEFAULT_IMAGE_ADDR + offset;

    addr_source += offset;
    image_len -= offset;

    if (image_len > 0)
    {
        ret = hal_flash_erase(addr_dest, image_len);
        if (ret != 0)
        {
            OTA_LOG("write inner flash failed");
            return OTA_ERRNO_INNER_FLASH_WRITE;
        }

        while (image_len > 0)
        {
            copy_len = image_len > OTA_COPY_BUF_SIZE ? OTA_COPY_BUF_SIZE : image_len;

            ret = hal_spi_flash_read(buf, copy_len, addr_source);
            if (ret != 0)
            {
                (void)hal_flash_lock();
                OTA_LOG("read spi flash failed");
                return OTA_ERRNO_SPI_FLASH_READ;
            }
            addr_source += copy_len;

            ret = hal_flash_write(buf, copy_len, &addr_dest);
            if (ret != 0)
            {
                OTA_LOG("write inner flash failed");
                return OTA_ERRNO_INNER_FLASH_WRITE;
            }

            offset += copy_len;
            if (offset % OTA_INNER_FLASH_BLOCK_SIZE == 0)
            {
                ret = func_set_update_record(state, offset);
                if (ret != 0)
                {
                    OTA_LOG("write spi flash failed");
                    return OTA_ERRNO_SPI_FLASH_WRITE;
                }
            }

            image_len -= copy_len;
        }
    }

    (void)hal_flash_lock();

    return OTA_ERRNO_OK;
}

static int prv_inner2spi_copy(int32_t image_len,
                              board_state state,
                              uint32_t offset,
                              int (*func_set_update_record)(uint8_t state, uint32_t offset))
{
    int ret;
    int32_t copy_len;
    uint8_t buf[OTA_COPY_BUF_SIZE];
    uint32_t addr_source = OTA_DEFAULT_IMAGE_ADDR + offset;
    uint32_t addr_dest = OTA_IMAGE_BCK_ADDR;

    image_len -= offset;

    ret = hal_spi_flash_erase(addr_dest, image_len);
    if (ret != 0)
    {
        OTA_LOG("write spi flash failed");
        return OTA_ERRNO_SPI_FLASH_WRITE;
    }

    while (image_len > 0)
    {
        copy_len = image_len > OTA_COPY_BUF_SIZE ? OTA_COPY_BUF_SIZE : image_len;

        ret = hal_flash_read(buf, copy_len, addr_source);
        if (ret != 0)
        {
            OTA_LOG("read inner flash failed");
            return OTA_ERRNO_INNER_FLASH_READ;
        }
        addr_source += copy_len;

        ret = hal_spi_flash_write(buf, copy_len, &addr_dest);
        if (ret != 0)
        {
            OTA_LOG("write spi flash failed");
            return OTA_ERRNO_SPI_FLASH_WRITE;
        }

        offset += copy_len;
        if (offset % OTA_RECORD_OFFSET_SIZE == 0)
        {
            ret = func_set_update_record(state, offset);
            if (ret != 0)
            {
                OTA_LOG("write spi flash failed");
                return OTA_ERRNO_SPI_FLASH_WRITE;
            }
        }

        image_len -= copy_len;
    }

    return OTA_ERRNO_OK;
}

int board_jump2app(void)
{
    jump_func jump;
    uint32_t pc = *(__IO uint32_t *)(OTA_DEFAULT_IMAGE_ADDR + 4);
    uint32_t stack = *(__IO uint32_t *)(OTA_DEFAULT_IMAGE_ADDR);

    if ((pc & OTA_PC_MASK) == OTA_FLASH_BASE)
    {
        if ((stack & OTA_STACK_MASK) == OTA_MEMORY_BASE)
        {
            jump = (jump_func)pc;
            set_msp(stack);
            jump();
        }
        else
        {
            OTA_LOG("stack value(%04X) of the image is ilegal", stack);
            return OTA_ERRNO_ILEGAL_STACK;
        }
    }
    else
    {
        OTA_LOG("PC value(%04X) of the image is ilegal", pc);
        return OTA_ERRNO_ILEGAL_PC;
    }

    return OTA_ERRNO_OK;
}

int board_update_copy(int32_t old_image_len, int32_t new_image_len,
                      uint32_t new_image_addr,
                      void (*func_get_update_record)(uint8_t *state, uint32_t *offset),
                      int (*func_set_update_record)(uint8_t state, uint32_t offset))
{
    int ret;
    board_state cur_state;
    uint32_t cur_offset;

    if (old_image_len < 0 || new_image_len < 0)
    {
        OTA_LOG("ilegal old_image_len(%d) or new_image_len(%d)", old_image_len, new_image_len);
        return OTA_ERRNO_ILEGAL_PARAM;
    }
    if (NULL == func_get_update_record || NULL == func_set_update_record)
    {
        OTA_LOG("function pointer of read/write ota_flag is null");
        return OTA_ERRNO_ILEGAL_PARAM;
    }

    func_get_update_record((uint8_t *)&cur_state, &cur_offset);

    if (cur_state <= BOARD_BCK)
    {
        if (cur_state == BOARD_INIT)
        {
            cur_state = BOARD_BCK;
            cur_offset = 0;
            ret = func_set_update_record((uint8_t)cur_state, cur_offset);
            if (ret != 0)
            {
                OTA_LOG("write ota flag failed");
                return OTA_ERRNO_SPI_FLASH_WRITE;
            }
        }
        ret = prv_inner2spi_copy(old_image_len, cur_state, cur_offset, func_set_update_record);
        if (ret != 0)
        {
            OTA_LOG("back up old image failed");
            return ret;
        }
    }

    if (cur_state != BOARD_UPDATE)
    {
        cur_state = BOARD_UPDATE;
        cur_offset = 0;
        ret = func_set_update_record((uint8_t)BOARD_UPDATE, cur_offset);
        if (ret != 0)
        {
            OTA_LOG("write ota flag failed");
            return OTA_ERRNO_SPI_FLASH_WRITE;
        }
    }
    ret = prv_spi2inner_copy(new_image_addr, new_image_len, cur_state, cur_offset, func_set_update_record);
    if (ret != 0)
    {
        OTA_LOG("update image failed");
        return ret;
    }
    ret = func_set_update_record((uint8_t)BOARD_INIT, 0);
    if (ret != 0)
    {
        OTA_LOG("write ota flag failed");
        return OTA_ERRNO_SPI_FLASH_WRITE;
    }

    return OTA_ERRNO_OK;
}

int board_rollback_copy(int32_t image_len,
                        void (*func_get_update_record)(uint8_t *record, uint32_t *offset),
                        int (*func_set_update_record)(uint8_t record, uint32_t offset))
{
    int ret;
    board_state cur_state;
    uint32_t cur_offset;

    if (image_len < 0)
    {
        OTA_LOG("ilegal image_len:%d", image_len);
        return OTA_ERRNO_ILEGAL_PARAM;
    }
    if (NULL == func_get_update_record || NULL == func_set_update_record)
    {
        OTA_LOG("function pointer of read/write ota_flag is null");
        return OTA_ERRNO_ILEGAL_PARAM;
    }

    func_get_update_record((uint8_t *)&cur_state, &cur_offset);

    if (cur_state != BOARD_ROLLBACK)
    {
        cur_state = BOARD_ROLLBACK;
        cur_offset = 0;
        ret = func_set_update_record((uint8_t)cur_state, cur_offset);
        if (ret != 0)
        {
            OTA_LOG("write ota flag failed");
            return OTA_ERRNO_SPI_FLASH_WRITE;
        }
    }
    ret = prv_spi2inner_copy(OTA_IMAGE_BCK_ADDR, image_len, cur_state, cur_offset, func_set_update_record);
    if (ret != 0)
    {
        OTA_LOG("rollback image failed");
        return ret;
    }
    ret = func_set_update_record((uint8_t)BOARD_INIT, 0);
    if (ret != 0)
    {
        OTA_LOG("write ota flag failed");
        return OTA_ERRNO_SPI_FLASH_WRITE;
    }

    return 0;
}