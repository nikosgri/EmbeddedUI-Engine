/**
 ******************************************************************************
 * @file    lfs_port.c
 * @brief   LittleFS port for MX25LM51245G via OctoSPI (STM32U5)
 *
 * ARCHITECTURE
 * ============
 *
 *  READ  → memory-mapped (0x90000000 + addr)
 *           Plain memcpy, zero command overhead. This is the same path used
 *           by NeoChrome/LVGL when loading images and fonts from flash.
 *
 *  PROG  → exit memory-mapped → WriteEnable + PageProgram +
 *           AutoPolling → re-enter memory-mapped →
 *           DCache invalidate for the written region.
 *
 *  ERASE → exit memory-mapped → WriteEnable + BlockErase(4K) +
 *           AutoPolling → re-enter memory-mapped →
 *           DCache invalidate for the erased block.
 *
 *  SYNC  → no-op (every prog/erase already waits for WIP=0 via AutoPolling).
 *
 * FLASH PARAMETERS (MX25LM51245G)
 * ================================
 *  Flash size     : 64 MB  (512 Mbit)
 *  Page size      : 256 B   <- minimum write unit
 *  Subsector (4K) : 4096 B  <- minimum erase unit  <- lfs block
 *  Sector (64K)   : 65536 B
 *  Block count    : 64*1024*1024 / 4096 = 16384 blocks
 *
 * DCACHE
 * ======
 *  The STM32U5 has D-Cache enabled (MX_DCACHE1_Init). After every prog/erase
 *  the affected cache lines must be invalidated so that the next read through
 *  the mapped pointer fetches fresh data from flash instead of stale cache.
 ******************************************************************************
 */

#include "lfs_port.h"
#include "octospi.h"          /* hospi1                                       */
#include "mx25lm51245g.h"     /* MX25LM51245G_* driver                        */
#include "dcache.h"           /* hdcache1 — STM32U5 DCache peripheral handle  */
#include <string.h>           /* memcpy                                       */

/* -------------------------------------------------------------------------
 * Flash geometry — from datasheet and mx25lm51245g.h definitions
 *
 * NOTE: The name FLASH_PAGE_SIZE is already taken by the STM32U5 HAL
 *       (stm32u599xx.h defines it as 0x2000 for the internal flash page).
 *       We use the prefix EXT_ to refer to the external NOR flash page.
 * -------------------------------------------------------------------------*/
#define FLASH_MAPPED_BASE     (0x90000000UL)                          /* OSPI1 AHB window      */
#define EXT_FLASH_PAGE_SIZE   (256U)                                  /* minimum write unit    */
#define FLASH_BLOCK_SIZE      (MX25LM51245G_SUBSECTOR_4K)             /* 4096 B — min erase    */
#define FLASH_TOTAL_SIZE      (MX25LM51245G_FLASH_SIZE)               /* 64 MB                 */
#define FLASH_BLOCK_COUNT     (FLASH_TOTAL_SIZE / FLASH_BLOCK_SIZE)   /* 16384 blocks          */

/* -------------------------------------------------------------------------
 * Static buffers — avoids heap usage and fragmentation on embedded targets
 * -------------------------------------------------------------------------*/
static uint8_t lfs_read_buf    [EXT_FLASH_PAGE_SIZE]; /* read cache  (256 B)     */
static uint8_t lfs_prog_buf    [EXT_FLASH_PAGE_SIZE]; /* prog cache  (256 B)     */
static uint8_t lfs_lookahead_buf[64];                 /* 64 B = tracks 512 blocks*/

/* -------------------------------------------------------------------------
 * Exposed instances (declared extern in the header)
 * -------------------------------------------------------------------------*/
lfs_t             lfs;
struct lfs_config lfs_cfg;

/* -------------------------------------------------------------------------
 * Internal forward declarations
 * -------------------------------------------------------------------------*/
extern OSPI_HandleTypeDef hospi1;
extern int32_t OSPI_NOR_EnableMemoryMappedMode(OSPI_HandleTypeDef *hospi);

/* =========================================================================
 * Helper functions for memory-mapped mode transitions
 * =========================================================================*/

static int _exit_mapped_mode(void)
{
    if (HAL_OSPI_Abort(&hospi1) != HAL_OK)
    {
        return LFS_ERR_IO;
    }
    HAL_Delay(1U);
    return LFS_ERR_OK;
}

static int _enter_mapped_mode(void)
{
    if (OSPI_NOR_EnableMemoryMappedMode(&hospi1) != 0)
    {
        return LFS_ERR_IO;
    }
    return LFS_ERR_OK;
}

/**
 * @brief  Invalidate D-Cache for a specific region of the mapped flash window.
 *
 * The STM32U5 DCACHE is a dedicated AHB peripheral, NOT the standard Cortex-M33
 * SCB cache. The correct API is HAL_DCACHE_InvalidateByAddr(), which requires:
 *  - start address aligned to 32 bytes (DCACHE line size)
 *  - size rounded up to the next multiple of 32 bytes
 *
 * Both alignments are computed automatically here.
 */
static void _invalidate_dcache(uint32_t flash_addr, uint32_t size)
{
    uint32_t mapped_addr  = FLASH_MAPPED_BASE + flash_addr;
    /* align start address down to the nearest 32-byte cache-line boundary */
    uint32_t aligned_addr = mapped_addr & ~0x1FUL;
    /* extend size to cover the alignment padding, then round up to 32 bytes */
    uint32_t aligned_size = size + (mapped_addr - aligned_addr);
    aligned_size = (aligned_size + 31U) & ~31U;

    HAL_DCACHE_InvalidateByAddr(&hdcache1, (const uint32_t *)aligned_addr, aligned_size);
}

/* =========================================================================
 * LittleFS callbacks
 * =========================================================================*/

/**
 * @brief  READ callback — reads directly via the memory-mapped pointer.
 *
 * LittleFS parameters:
 *  block : block number (0-based)
 *  off   : byte offset within the block
 *  size  : number of bytes to read
 */
static int lfs_port_read(const struct lfs_config *c,
                         lfs_block_t block,
                         lfs_off_t   off,
                         void       *buffer,
                         lfs_size_t  size)
{
    (void)c;  /* context not needed here */

    /* Compute absolute flash address */
    uint32_t addr = (block * FLASH_BLOCK_SIZE) + off;

    /* Read directly from the mapped window — no command overhead */
    const uint8_t *src = (const uint8_t *)(FLASH_MAPPED_BASE + addr);
    memcpy(buffer, src, size);

    return LFS_ERR_OK;
}

/**
 * @brief  PROG callback — programs one page (or a partial page) to flash.
 *
 * LittleFS guarantees:
 *  - prog is only called on a previously erased block
 *  - size is always a multiple of prog_size (256 B)
 *
 * If size > 256 B, the write is split into multiple 256-byte PageProgram calls.
 */
static int lfs_port_prog(const struct lfs_config *c,
                         lfs_block_t  block,
                         lfs_off_t    off,
                         const void  *buffer,
                         lfs_size_t   size)
{
    (void)c;

    uint32_t       addr      = (block * FLASH_BLOCK_SIZE) + off;
    const uint8_t *src       = (const uint8_t *)buffer;
    uint32_t       remaining = size;

    /* Exit memory-mapped mode to issue commands */
    int ret = _exit_mapped_mode();
    if (ret != LFS_ERR_OK) return ret;

    while (remaining > 0U)
    {
        /* PageProgram writes max 256 bytes and must not cross a page boundary */
        uint32_t chunk = remaining;
        if (chunk > FLASH_PAGE_SIZE)
        {
            chunk = FLASH_PAGE_SIZE;
        }

        /* WriteEnable must be issued before every PageProgram */
        if (MX25LM51245G_WriteEnable(&hospi1,
                                      MX25LM51245G_OPI_MODE,
                                      MX25LM51245G_STR_TRANSFER) != MX25LM51245G_OK)
        {
            _enter_mapped_mode();
            return LFS_ERR_IO;
        }

        if (MX25LM51245G_PageProgram(&hospi1,
                                      MX25LM51245G_OPI_MODE,
                                      MX25LM51245G_4BYTES_SIZE,
                                      (uint8_t *)src,
                                      addr,
                                      chunk) != MX25LM51245G_OK)
        {
            _enter_mapped_mode();
            return LFS_ERR_IO;
        }

        /* Wait for program completion (WIP bit polling) */
        if (MX25LM51245G_AutoPollingMemReady(&hospi1,
                                              MX25LM51245G_OPI_MODE,
                                              MX25LM51245G_STR_TRANSFER) != MX25LM51245G_OK)
        {
            _enter_mapped_mode();
            return LFS_ERR_IO;
        }

        src       += chunk;
        addr      += chunk;
        remaining -= chunk;
    }

    /* Restore memory-mapped mode */
    ret = _enter_mapped_mode();
    if (ret != LFS_ERR_OK) return ret;

    /* Invalidate D-Cache for the region just written */
    uint32_t start_addr = (block * FLASH_BLOCK_SIZE) + off;
    _invalidate_dcache(start_addr, size);

    return LFS_ERR_OK;
}

/**
 * @brief  ERASE callback — erases one 4 KB subsector.
 *
 * LittleFS calls erase with block granularity (one block at a time).
 * block_size is set to 4 KB, matching the smallest erasable unit.
 */
static int lfs_port_erase(const struct lfs_config *c, lfs_block_t block)
{
    (void)c;

    uint32_t addr = block * FLASH_BLOCK_SIZE;

    /* Exit memory-mapped mode to issue commands */
    int ret = _exit_mapped_mode();
    if (ret != LFS_ERR_OK) return ret;

    if (MX25LM51245G_WriteEnable(&hospi1,
                                  MX25LM51245G_OPI_MODE,
                                  MX25LM51245G_STR_TRANSFER) != MX25LM51245G_OK)
    {
        _enter_mapped_mode();
        return LFS_ERR_IO;
    }

    if (MX25LM51245G_BlockErase(&hospi1,
                                 MX25LM51245G_OPI_MODE,
                                 MX25LM51245G_STR_TRANSFER,
                                 MX25LM51245G_4BYTES_SIZE,
                                 addr,
                                 MX25LM51245G_ERASE_4K) != MX25LM51245G_OK)
    {
        _enter_mapped_mode();
        return LFS_ERR_IO;
    }

    /* Wait for erase completion (WIP bit polling) */
    if (MX25LM51245G_AutoPollingMemReady(&hospi1,
                                          MX25LM51245G_OPI_MODE,
                                          MX25LM51245G_STR_TRANSFER) != MX25LM51245G_OK)
    {
        _enter_mapped_mode();
        return LFS_ERR_IO;
    }

    /* Restore memory-mapped mode */
    ret = _enter_mapped_mode();
    if (ret != LFS_ERR_OK) return ret;

    /* Invalidate D-Cache for the erased block */
    _invalidate_dcache(addr, FLASH_BLOCK_SIZE);

    return LFS_ERR_OK;
}

/**
 * @brief  SYNC callback — no-op.
 *
 * Every prog/erase call already waits for WIP=0 via AutoPolling,
 * so no additional flush is needed.
 */
static int lfs_port_sync(const struct lfs_config *c)
{
    (void)c;
    return LFS_ERR_OK;
}

/* =========================================================================
 * lfs_port_init — fills in lfs_cfg
 * =========================================================================*/

void lfs_port_init(void)
{
    lfs_cfg.context = NULL;  /* no context needed */

    /* Callbacks */
    lfs_cfg.read  = lfs_port_read;
    lfs_cfg.prog  = lfs_port_prog;
    lfs_cfg.erase = lfs_port_erase;
    lfs_cfg.sync  = lfs_port_sync;

    /* Flash geometry */
    lfs_cfg.read_size      = 1U;                  /* memory-mapped: any size is valid */
    lfs_cfg.prog_size      = EXT_FLASH_PAGE_SIZE; /* 256 B — page granularity         */
    lfs_cfg.block_size     = FLASH_BLOCK_SIZE;    /* 4096 B — subsector               */
    lfs_cfg.block_count    = FLASH_BLOCK_COUNT;   /* 16384 blocks = 64 MB             */

    /* Wear leveling — rotate metadata every 500 erase cycles */
    lfs_cfg.block_cycles   = 500;

    /* Cache size matches prog_size so one cache entry covers one page */
    lfs_cfg.cache_size     = EXT_FLASH_PAGE_SIZE; /* 256 B */
    lfs_cfg.lookahead_size = 64U;                 /* 64 * 8 bits = 512 blocks tracked */

    /* Static buffers — avoids heap allocation */
    lfs_cfg.read_buffer      = lfs_read_buf;
    lfs_cfg.prog_buffer      = lfs_prog_buf;
    lfs_cfg.lookahead_buffer = lfs_lookahead_buf;
}
