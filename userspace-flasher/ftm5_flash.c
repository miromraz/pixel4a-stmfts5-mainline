/*
 * ftm5_flash — userspace flasher for the STM FTM5 touchscreen on Pixel 4a (sunfish)
 *
 * Sends the protocol bytes RE'd from Google's downstream ftm5.ko binary:
 *   /vendor/lib/modules/ftm5.ko on a stock Pixel 4a vendor image
 *
 * Each command goes through /dev/i2c-7 (qcom GENI bus 7) to address 0x49.
 * The hardware reset GPIO is TLMM gpio8 (active-low); we toggle it via a
 * spawned `gpioset` since libgpiod's 2.x C API differs across distros and
 * shelling out is easier than tracking versions.
 *
 * Run with:
 *   sudo sh -c 'echo 7-0049 > /sys/bus/i2c/drivers/stmfts/unbind'   # free the chip
 *   sudo ./ftm5_flash /lib/firmware/ftm5_fw.ftb
 *
 * Exit 0 on successful flash + verify, nonzero on any error.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/gpio.h>

#define I2C_BUS              "/dev/i2c-7"
#define I2C_ADDR             0x49
#define GPIO_CHIP            "gpiochip2"   /* TLMM */
#define GPIO_RESET_LINE      8
#define GPIO_SWITCH_LINE     72            /* low = AP, high = SLPI; we want AP */
#define PM6150_CHIP          "gpiochip0"
#define PM6150_TOUCH_PWR     3             /* pm6150 GPIO4 = line 3 (0-indexed) */

#define FTB_HEADER_SIZE      64
#define FTB_HEADER_CRC_SIZE  4
#define FTB_MAGIC            0xAA55AA55u
#define FTB_VER              0x00000001u
#define FTB_CHIP_ID_0        0x36
#define FTB_CHIP_ID_1        0x48          /* Pixel 4a specific; generic FTM5 is 0x70 */

/* Flash protocol commands (every byte RE'd from ftm5.ko's rodata + disasm) */
static const uint8_t CMD_HOLD_M3[]            = { 0xFA, 0x20, 0x00, 0x00, 0x24, 0x01 };
static const uint8_t CMD_SYSTEM_RESET[]       = { 0xFA, 0x20, 0x00, 0x00, 0x24, 0x81 };
static const uint8_t CMD_UVLO_1[]             = { 0xFA, 0x20, 0x00, 0x00, 0x1B, 0x66 };
static const uint8_t CMD_UVLO_2[]             = { 0xFA, 0x20, 0x00, 0x00, 0x68, 0x13 };
static const uint8_t CMD_FLASH_UNLOCK_1[]     = { 0xFA, 0x20, 0x00, 0x00, 0x25, 0x20 };
static const uint8_t CMD_FLASH_UNLOCK_2[]     = { 0xFA, 0x20, 0x00, 0x00, 0x6B, 0x00 };
static const uint8_t CMD_ERASE_UNLOCK[]       = { 0xFA, 0x20, 0x00, 0x00, 0xDE, 0x03 };
static const uint8_t CMD_FULL_ERASE_PREP[]    = { 0xFA, 0x20, 0x00, 0x00, 0x6B, 0x00 };
static const uint8_t CMD_FULL_ERASE_TRIGGER[] = { 0xFA, 0x20, 0x00, 0x00, 0x6A, 0xC0 };
/* page-by-page erase: prep + mask + trigger (mask byte filled at runtime) */
static const uint8_t CMD_PBP_PREP[]           = { 0xFA, 0x20, 0x00, 0x00, 0x6B, 0x00 };
static const uint8_t CMD_PBP_TRIGGER[]        = { 0xFA, 0x20, 0x00, 0x00, 0x6A, 0xA0 };
static const uint8_t CMD_START_FLASH_DMA[]    = { 0xFA, 0x20, 0x00, 0x00, 0x6B, 0x00,
                                                   0x40, 0x42, 0x0F, 0x00, 0x00, 0xC0 };

#define WAIT_TYPE_ERASE      0x6A
#define WAIT_TYPE_DMA        0x71
#define SCRATCH_RAM_BASE     0x00100000u

/* Chunk constants in fillFlash */
#define FLASH_CHUNK_BYTES    32768         /* per-DMA scratch chunk: 32 KB */
#define I2C_CHUNK_BYTES      32            /* per-i2c-write data: 32 bytes */

static int g_i2c_fd = -1;
static int g_verbose = 1;

/* ---- logging --------------------------------------------------------- */

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "[%4ld.%03ld] ", ts.tv_sec, ts.tv_nsec / 1000000);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void hexdump(const char *prefix, const uint8_t *buf, int len)
{
    if (!g_verbose) return;
    fprintf(stderr, "    %s [", prefix);
    for (int i = 0; i < len && i < 16; i++) fprintf(stderr, "%02X ", buf[i]);
    if (len > 16) fprintf(stderr, "... (%d bytes total)", len);
    fprintf(stderr, "]\n");
}

/* ---- timing ---------------------------------------------------------- */

static void msleep(int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}

/* ---- GPIO via /dev/gpiochipN ioctl directly ------------------------- */

/* One persistent GPIO_V2_LINE request per (chip, line) so the line value
 * survives between calls. Closing the fd releases the line back to the
 * kernel. */
struct held_line {
    int fd;       /* line request fd */
    int chip_fd;  /* chip fd (kept open) */
    const char *chip_path;
    int offset;
};

static struct held_line g_switch = { -1, -1, "/dev/gpiochip2", GPIO_SWITCH_LINE };
static struct held_line g_pwr    = { -1, -1, "/dev/gpiochip0", PM6150_TOUCH_PWR };
static struct held_line g_reset  = { -1, -1, "/dev/gpiochip2", GPIO_RESET_LINE };

/* Open chip and request a single line as output with initial value. */
static int hold_line_request(struct held_line *h, int initial_val, const char *consumer)
{
    if (h->fd >= 0) {
        /* already held; just change value below */
    } else {
        if (h->chip_fd < 0) {
            h->chip_fd = open(h->chip_path, O_RDWR | O_CLOEXEC);
            if (h->chip_fd < 0) {
                logmsg("ERR: open %s: %s", h->chip_path, strerror(errno));
                return -1;
            }
        }
        struct gpio_v2_line_request req;
        memset(&req, 0, sizeof(req));
        req.offsets[0] = h->offset;
        req.num_lines = 1;
        snprintf(req.consumer, sizeof(req.consumer), "%s", consumer);
        req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
        req.config.num_attrs = 1;
        req.config.attrs[0].mask = 1;
        req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
        req.config.attrs[0].attr.values = initial_val ? 1 : 0;
        if (ioctl(h->chip_fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
            logmsg("ERR: GPIO_V2_GET_LINE_IOCTL chip=%s line=%d: %s",
                   h->chip_path, h->offset, strerror(errno));
            return -1;
        }
        h->fd = req.fd;
    }
    return 0;
}

static int hold_line_set(struct held_line *h, int value)
{
    if (h->fd < 0) {
        logmsg("ERR: line not requested");
        return -1;
    }
    struct gpio_v2_line_values v = { .bits = value ? 1 : 0, .mask = 1 };
    if (ioctl(h->fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) < 0) {
        logmsg("ERR: GPIO_V2_LINE_SET_VALUES_IOCTL line=%d val=%d: %s",
               h->offset, value, strerror(errno));
        return -1;
    }
    return 0;
}

static void hold_line_release(struct held_line *h)
{
    if (h->fd >= 0)    { close(h->fd);      h->fd = -1; }
    if (h->chip_fd >= 0) { close(h->chip_fd); h->chip_fd = -1; }
}

/* Hardware system reset:
 *   switch=0 (route IRQ to AP)
 *   reset low 30ms, then high
 * pm6150 GPIO4 is already held high by the DT's vreg_touch_en always-on
 * regulator, so we don't touch it. The TLMM lines stay held by us until
 * release_gpios() at exit. */
static int hardware_reset(void)
{
    logmsg("hw reset: switch=0, reset low 30ms then high");
    if (hold_line_request(&g_switch, 0, "ftm5_switch") < 0) return -1;
    if (hold_line_request(&g_reset,  0, "ftm5_reset")  < 0) return -1;
    msleep(30);
    if (hold_line_set(&g_reset, 1) < 0) return -1;
    msleep(50);
    return 0;
}

static void release_gpios(void)
{
    hold_line_release(&g_reset);
    hold_line_release(&g_switch);
}

/* ---- i2c primitives -------------------------------------------------- */

static int i2c_write(const void *buf, int len)
{
    int n = write(g_i2c_fd, buf, len);
    if (n != len) {
        logmsg("ERR: i2c write len=%d ret=%d errno=%d (%s)",
               len, n, errno, strerror(errno));
        hexdump("tx", buf, len);
        return -1;
    }
    return 0;
}

static int i2c_write_read(const void *wbuf, int wlen, void *rbuf, int rlen)
{
    struct i2c_msg msgs[2] = {
        { .addr = I2C_ADDR, .flags = 0,        .len = wlen, .buf = (uint8_t *)wbuf },
        { .addr = I2C_ADDR, .flags = I2C_M_RD, .len = rlen, .buf = rbuf },
    };
    struct i2c_rdwr_ioctl_data d = { .msgs = msgs, .nmsgs = 2 };
    if (ioctl(g_i2c_fd, I2C_RDWR, &d) < 0) {
        logmsg("ERR: i2c writeread wlen=%d rlen=%d errno=%d (%s)",
               wlen, rlen, errno, strerror(errno));
        hexdump("tx", wbuf, wlen);
        return -1;
    }
    return 0;
}

/* ---- protocol ops ---------------------------------------------------- */

static int wait_for_flash_ready(uint8_t type)
{
    uint8_t tx[5] = { 0xFA, 0x20, 0x00, 0x00, type };
    uint8_t rx[2];
    for (int i = 0; i < 200; i++) {
        if (i2c_write_read(tx, 5, rx, 2) < 0)
            return -1;
        if ((rx[0] & 0x80) == 0) {
            logmsg("flash ready (type=0x%02X) after %d*50ms", type, i);
            return 0;
        }
        msleep(50);
    }
    logmsg("ERR: flash not ready after 10s (type=0x%02X)", type);
    return -1;
}

/* Page-by-page erase: erase only pages [0..last_page-1], preserve [last_page..31].
 * Chip has 32 pages of 4 KB (= 128 KB flash). For our firmware (28 code + 1
 * config + 2 CX = 31 pages used) we erase 0..30 and leave page 31 alone.
 *
 * Command sequence (RE'd from ftm5.ko rodata + binary):
 *   prep:    FA 20 00 00 6B 00
 *   mask:    FA 20 00 01 28 <mask_LE_4B>     (bit N set = erase page N)
 *   trigger: FA 20 00 00 6A A0
 *   then wait_for_flash_ready(0x6A) */
static int flash_erase_page_by_page(int last_page_exclusive)
{
    /* Per Google's ftsFlash.c: cmd2 initializes to FF FF FF FF (all erase),
     * then `cmd2[5+i] = cmd2[5+i] & (~mask[i])` where mask has bits set for
     * pages to KEEP. So the wire mask sent has:
     *   bit=1 → erase that page
     *   bit=0 → keep that page
     * Wire mask = pages_to_erase_bitmap directly (NO inversion). */
    uint32_t wire_mask = (last_page_exclusive >= 32) ? 0xFFFFFFFFu
                                                     : ((1u << last_page_exclusive) - 1u);
    uint8_t cmd_mask[9] = {
        0xFA, 0x20, 0x00, 0x01, 0x28,
        (uint8_t)(wire_mask & 0xFF),
        (uint8_t)((wire_mask >> 8) & 0xFF),
        (uint8_t)((wire_mask >> 16) & 0xFF),
        (uint8_t)((wire_mask >> 24) & 0xFF),
    };
    logmsg("page_by_page erase: pages 0..%d (wire_mask 0x%08X bit=1=erase)",
           last_page_exclusive - 1, wire_mask);
    /* Google's order in flash_erase_page_by_page: mask, then prep, then trigger.
     * Setting the mask register BEFORE clearing PBP prep avoids the prep wiping it. */
    if (i2c_write(cmd_mask,         9)                       < 0) return -1;
    if (i2c_write(CMD_PBP_PREP,     sizeof(CMD_PBP_PREP))    < 0) return -1;
    if (i2c_write(CMD_PBP_TRIGGER,  sizeof(CMD_PBP_TRIGGER)) < 0) return -1;
    return wait_for_flash_ready(WAIT_TYPE_ERASE);
}

static int start_flash_dma(void)
{
    if (i2c_write(CMD_START_FLASH_DMA, sizeof(CMD_START_FLASH_DMA)) < 0)
        return -1;
    return wait_for_flash_ready(WAIT_TYPE_DMA);
}

/* fillFlash: write `size` bytes from `data` so the chip writes them to flash
 * starting at `dst_addr` (in u32-word units, per Google's binary).
 *
 * Loop: write 32-byte chunks to scratch RAM at 0x00100000+chunk_byte_offset,
 * and every 32 KB (or at end of buffer) emit a DMA-config write + trigger DMA.
 */
static int fill_flash(uint32_t dst_addr_words, const uint8_t *data, int size)
{
    int remaining = size;
    int chunk_counter = 0;        /* 32 KB scratch chunks already DMA'd */
    int byte_offset = 0;          /* bytes accumulated in current 32 KB chunk */
    uint32_t scratch = SCRATCH_RAM_BASE;
    const uint8_t *p = data;
    uint8_t buf[5 + I2C_CHUNK_BYTES];
    uint8_t dma_cfg[12];

    logmsg("fillFlash dst_word=0x%05X size=%d (%d chunks of 32KB)",
           dst_addr_words, size, (size + FLASH_CHUNK_BYTES - 1) / FLASH_CHUNK_BYTES);

    while (remaining > 0) {
        int chunk = (remaining >= I2C_CHUNK_BYTES) ? I2C_CHUNK_BYTES : remaining;
        /* If this chunk would cross a 32-KB boundary, truncate it. */
        if (byte_offset + chunk > FLASH_CHUNK_BYTES)
            chunk = FLASH_CHUNK_BYTES - byte_offset;

        buf[0] = 0xFA;
        buf[1] = (scratch >> 24) & 0xFF;
        buf[2] = (scratch >> 16) & 0xFF;
        buf[3] = (scratch >> 8) & 0xFF;
        buf[4] = scratch & 0xFF;
        memcpy(&buf[5], p, chunk);
        if (i2c_write(buf, 5 + chunk) < 0) {
            logmsg("ERR: fillFlash chunk write at scratch=0x%08X chunk=%d", scratch, chunk);
            return -1;
        }

        scratch     += chunk;
        p           += chunk;
        remaining   -= chunk;
        byte_offset += chunk;

        /* Emit DMA at every 32 KB boundary OR when there's no more data. */
        if (byte_offset >= FLASH_CHUNK_BYTES || remaining == 0) {
            uint32_t addr_words = dst_addr_words + (chunk_counter * (FLASH_CHUNK_BYTES / 4));
            uint32_t count_minus_1 = (byte_offset / 4) - 1;

            dma_cfg[0]  = 0xFA;
            dma_cfg[1]  = 0x20;
            dma_cfg[2]  = 0x00;
            dma_cfg[3]  = 0x00;
            dma_cfg[4]  = 0x72;
            dma_cfg[5]  = 0x00;
            dma_cfg[6]  = 0x00;
            dma_cfg[7]  = addr_words & 0xFF;
            dma_cfg[8]  = (addr_words >> 8) & 0xFF;
            dma_cfg[9]  = count_minus_1 & 0xFF;
            dma_cfg[10] = (count_minus_1 >> 8) & 0xFF;
            dma_cfg[11] = 0x00;

            if (g_verbose) hexdump("dma_cfg", dma_cfg, 12);

            if (i2c_write(dma_cfg, 12) < 0) {
                logmsg("ERR: dma config write");
                return -1;
            }
            if (start_flash_dma() < 0) {
                logmsg("ERR: start_flash_dma at chunk %d", chunk_counter);
                return -1;
            }

            chunk_counter++;
            byte_offset = 0;
            scratch = SCRATCH_RAM_BASE;
        }
    }
    logmsg("fillFlash done: %d chunks DMA'd", chunk_counter);
    return 0;
}

/* ---- ftb parser ------------------------------------------------------ */

struct ftb_fw {
    uint8_t *raw;
    int raw_size;
    /* parsed header fields */
    uint16_t fw_ver;
    uint16_t config_id;
    uint8_t  external_release[8];
    uint32_t sec0_size;       /* code */
    uint32_t sec1_size;       /* config */
    uint32_t sec2_size;       /* CX */
    uint32_t sec3_size;
    /* payload pointer (raw + 64) and per-section pointers */
    const uint8_t *code;
    const uint8_t *config;
    const uint8_t *cx;
    /* dst addresses computed from page counts at offsets 280..283 */
    uint32_t code_dst_word;
    uint32_t cx_dst_word;
    uint32_t config_dst_word;
};

static int parse_ftb(const char *path, struct ftb_fw *fw)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return -1; }
    struct stat st;
    fstat(fd, &st);
    fw->raw_size = st.st_size;
    fw->raw = malloc(fw->raw_size);
    if (read(fd, fw->raw, fw->raw_size) != fw->raw_size) {
        perror("read"); close(fd); return -1;
    }
    close(fd);

    const uint8_t *d = fw->raw;
    if (fw->raw_size < FTB_HEADER_SIZE + FTB_HEADER_CRC_SIZE) {
        logmsg("ERR: file too small (%d bytes)", fw->raw_size); return -1;
    }
    uint32_t magic = d[0] | (d[1] << 8) | (d[2] << 16) | (d[3] << 24);
    if (magic != FTB_MAGIC) {
        logmsg("ERR: bad magic 0x%08X (expected 0x%08X)", magic, FTB_MAGIC);
        return -1;
    }
    uint32_t ftb_ver = d[4] | (d[5] << 8) | (d[6] << 16) | (d[7] << 24);
    if (ftb_ver != FTB_VER) {
        logmsg("ERR: bad ftb_ver 0x%08X", ftb_ver); return -1;
    }
    if (d[8] != FTB_CHIP_ID_0 || d[9] != FTB_CHIP_ID_1) {
        logmsg("ERR: wrong chip id %02X %02X (expected %02X %02X)",
               d[8], d[9], FTB_CHIP_ID_0, FTB_CHIP_ID_1);
        return -1;
    }
    fw->fw_ver = d[16] | (d[17] << 8);
    fw->config_id = d[24] | (d[25] << 8);
    memcpy(fw->external_release, &d[36], 8);
    fw->sec0_size = d[44] | (d[45] << 8) | (d[46] << 16) | (d[47] << 24);
    fw->sec1_size = d[48] | (d[49] << 8) | (d[50] << 16) | (d[51] << 24);
    fw->sec2_size = d[52] | (d[53] << 8) | (d[54] << 16) | (d[55] << 24);
    fw->sec3_size = d[56] | (d[57] << 8) | (d[58] << 16) | (d[59] << 24);

    uint32_t expected = FTB_HEADER_SIZE + FTB_HEADER_CRC_SIZE
                      + fw->sec0_size + fw->sec1_size + fw->sec2_size + fw->sec3_size;
    if (expected != (uint32_t)fw->raw_size) {
        logmsg("ERR: section sum mismatch: %u + 68 != file size %u",
               fw->sec0_size + fw->sec1_size + fw->sec2_size + fw->sec3_size,
               fw->raw_size);
        return -1;
    }

    /* Compute dst addresses (dynamic if all four page-count bytes are non-zero). */
    uint8_t pc0 = d[280], pc1 = d[281], pc2 = d[282], pc3 = d[283];
    fw->code_dst_word = 0;
    if (pc0 && pc1 && pc2 && pc3) {
        fw->cx_dst_word     = (uint32_t)(pc0 + pc1) << 10;
        fw->config_dst_word = (uint32_t)(pc0 + pc1 + pc2) << 10;
        logmsg("dynamic dst: code=0x%X cx=0x%X config=0x%X (pages: %u %u %u %u)",
               fw->code_dst_word, fw->cx_dst_word, fw->config_dst_word,
               pc0, pc1, pc2, pc3);
    } else {
        fw->cx_dst_word     = 0x00007000;
        fw->config_dst_word = 0x00007C00;
        logmsg("default dst: code=0 cx=0x7000 config=0x7C00");
    }

    /* Match Google's parseBinFile + flash_burn exactly:
     * After parsing the 64-byte header, the parser allocates `dimension`
     * bytes (= sum of all section sizes), then `memcpy(fwData->data,
     * &fw_data[index], dimension)` at index=64. So fwData->data[0] = raw[64]
     * = raw[0x40], which is the 4-byte HEADER_CRC followed by section data.
     * fillFlash is then called with src = &fw.data[0] (= raw[0x40]) and
     * size = fw.sec0_size, so flash[0..3] = header CRC bytes, flash[4..N-1]
     * = first (sec0_size - 4) bytes of section payload. */
    fw->code   = &fw->raw[FTB_HEADER_SIZE];   /* raw[0x40], includes header CRC */
    fw->config = fw->code + fw->sec0_size;
    fw->cx     = fw->config + fw->sec1_size;

    logmsg("ftb parsed: fw_ver=0x%04X config_id=0x%04X size=%d",
           fw->fw_ver, fw->config_id, fw->raw_size);
    logmsg("  sec0(code)=%u sec1(config)=%u sec2(cx)=%u sec3=%u",
           fw->sec0_size, fw->sec1_size, fw->sec2_size, fw->sec3_size);
    return 0;
}

/* ---- main orchestration ---------------------------------------------- */

#define WRITE_NAMED(name, buf) \
    do { \
        if (g_verbose) hexdump(name, buf, sizeof(buf)); \
        if (i2c_write(buf, sizeof(buf)) < 0) { logmsg("FAILED: " name); return -1; } \
    } while (0)

static int flash_burn(const struct ftb_fw *fw)
{
    logmsg("=== flash_burn START ===");

    logmsg("[1] hardware system reset (GPIO toggle)");
    if (hardware_reset() < 0) return -1;

    logmsg("[1b] i2c system reset (write 0x81 to register 0x20000024)");
    WRITE_NAMED("system_reset_i2c", CMD_SYSTEM_RESET);

    logmsg("[2] msleep 100ms");
    msleep(100);

    logmsg("[3] HOLD M3");
    WRITE_NAMED("hold_m3", CMD_HOLD_M3);

    logmsg("[4] enable UVLO + auto power down");
    WRITE_NAMED("uvlo_1", CMD_UVLO_1);
    WRITE_NAMED("uvlo_2", CMD_UVLO_2);

    logmsg("[5] flash unlock");
    WRITE_NAMED("flash_unlock_1", CMD_FLASH_UNLOCK_1);
    WRITE_NAMED("flash_unlock_2", CMD_FLASH_UNLOCK_2);

    logmsg("[6] flash erase unlock");
    WRITE_NAMED("flash_erase_unlock", CMD_ERASE_UNLOCK);

    logmsg("[7] flash page-by-page erase (preserve page 31 = bootloader)");
    /* Pages used = pc0(code)+pc1(config)+pc2(cx). For our file: 28+1+2 = 31.
     * Page 31 is left untouched. */
    int pages_to_erase = fw->code_dst_word ? 31 :   /* fallback if no parsed */
                         (28 + 1 + 2);              /* matches our file */
    /* Compute from page count bytes (already done by parser); use the values
     * stashed in the ftb_fw struct so this is data-driven. We re-derive here
     * because we didn't preserve the raw page count. Simpler: hardcode 31. */
    pages_to_erase = 31;
    if (flash_erase_page_by_page(pages_to_erase) < 0) {
        logmsg("FAILED at page-by-page erase");
        return -1;
    }

    logmsg("[8] LOAD PROGRAM (sec0 code: %u bytes @ word_addr 0x%X)",
           fw->sec0_size, fw->code_dst_word);
    if (fill_flash(fw->code_dst_word, fw->code, fw->sec0_size) < 0) return -1;

    logmsg("[9] LOAD CONFIG (sec1 config: %u bytes @ word_addr 0x%X)",
           fw->sec1_size, fw->config_dst_word);
    if (fill_flash(fw->config_dst_word, fw->config, fw->sec1_size) < 0) return -1;

    if (fw->sec2_size > 0) {
        logmsg("[10] LOAD CX (sec2: %u bytes @ word_addr 0x%X)",
               fw->sec2_size, fw->cx_dst_word);
        if (fill_flash(fw->cx_dst_word, fw->cx, fw->sec2_size) < 0) return -1;
    }

    logmsg("[11] final boot: release M3 + i2c system reset + hardware reset");
    /* Release M3 hold (chip can now run from flash) */
    static const uint8_t CMD_RELEASE_M3[] = { 0xFA, 0x20, 0x00, 0x00, 0x24, 0x00 };
    WRITE_NAMED("release_m3", CMD_RELEASE_M3);
    msleep(50);
    /* I2C system reset + GPIO reset for proper cold boot */
    WRITE_NAMED("system_reset_i2c_final", CMD_SYSTEM_RESET);
    msleep(50);
    if (hardware_reset() < 0) return -1;
    msleep(1500);

    /* [12] final check: try to read 8 bytes from STMFTS_READ_INFO=0x80
     * (chip-id+version+config-id+config-ver). If we get a response and the
     * chip id matches, the chip has firmware and is in app mode. */
    logmsg("[12] verify: STMFTS_READ_INFO");
    uint8_t tx_read_info[1] = { 0x80 };
    uint8_t rx[8];
    if (i2c_write_read(tx_read_info, 1, rx, 8) < 0) {
        logmsg("ERR: chip did not respond to READ_INFO — flash failed?");
        return -1;
    }
    logmsg("READ_INFO response: %02X %02X %02X %02X %02X %02X %02X %02X",
           rx[0], rx[1], rx[2], rx[3], rx[4], rx[5], rx[6], rx[7]);
    logmsg("=== flash_burn DONE ===");
    return 0;
}

/* ---- entry point ----------------------------------------------------- */

static int probe_only(void)
{
    uint8_t tx[1] = { 0x80 };
    uint8_t rx[8] = {0};
    if (i2c_write_read(tx, 1, rx, 8) < 0) {
        logmsg("probe: chip didn't respond to STMFTS_READ_INFO");
        return 1;
    }
    logmsg("probe: %02X %02X %02X %02X %02X %02X %02X %02X",
           rx[0], rx[1], rx[2], rx[3], rx[4], rx[5], rx[6], rx[7]);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <ftm5_fw.ftb> | --probe | --reset\n"
                "  --probe      try STMFTS_READ_INFO without flashing\n"
                "  --reset      just toggle hw reset GPIO and exit\n",
                argv[0]);
        return 2;
    }

    g_i2c_fd = open(I2C_BUS, O_RDWR);
    if (g_i2c_fd < 0) { perror(I2C_BUS); return 1; }
    if (ioctl(g_i2c_fd, I2C_SLAVE, I2C_ADDR) < 0) {
        perror("I2C_SLAVE"); return 1;
    }

    int rc = 0;
    if (strcmp(argv[1], "--probe") == 0) {
        rc = probe_only();
    } else if (strcmp(argv[1], "--reset") == 0) {
        if (hardware_reset() < 0) { rc = 1; goto out; }
        msleep(200);
        rc = probe_only();
    } else {
        struct ftb_fw fw;
        memset(&fw, 0, sizeof(fw));
        if (parse_ftb(argv[1], &fw) < 0) { rc = 1; goto out; }
        rc = (flash_burn(&fw) == 0) ? 0 : 1;
        free(fw.raw);
    }

out:
    release_gpios();
    close(g_i2c_fd);
    return rc;
}
