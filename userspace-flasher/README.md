# ftm5_flash — userspace FTM5 firmware flasher

Self-contained static aarch64 binary that implements the full
STMicroelectronics FTM5 firmware update protocol over I2C. Built for
Pixel 4a (sunfish) but the protocol is generic across FTM5 variants.

## Build

```
aarch64-linux-gnu-gcc -static -O2 -o ftm5_flash ftm5_flash.c
```

## Use

```
./ftm5_flash <ftm5_fw.ftb>     # flash a firmware blob
./ftm5_flash --probe           # try STMFTS_READ_INFO without flashing
./ftm5_flash --reset           # toggle hw reset GPIO and exit
```

Defaults assume `/dev/i2c-7`, I2C address `0x49`, reset GPIO on
gpiochip2 line 8, AP/SLPI mode-switch on gpiochip2 line 72. Adjust the
constants at the top of the source for other targets.

Requires root (raw `/dev/i2c-*` and `/dev/gpiochip*` access).

## Protocol notes

Reverse-engineered from Google's downstream `ftm5.ko` and cross-checked
against the GPL'd reference driver at
[kerneltoast/floral fts_lib/ftsFlash.c](https://github.com/kerneltoast/android_kernel_google_floral/blob/11.0.0-sultan/drivers/input/touchscreen/fts_touch/fts_lib/ftsFlash.c).

Critical findings during bringup:

1. **Erase mask polarity:** the chip's erase-page register accepts a 32-bit
   mask where **bit=1 erases that page, bit=0 keeps it**. The wire bytes
   are computed as `(initial_FF_FF_FF_FF) AND ~(pages_to_keep_bitmap)`.
   Easy to invert based on disassembly alone — got caught on this for
   several sessions before finding the GPL source.

2. **Erase command order:** mask write → PBP prep → PBP trigger. Reversing
   prep ↔ mask silently no-ops the erase.

3. **DMA word-count semantics:** `count_minus_1 = (byte_count / 4) - 1`
   (i.e., count register holds number-of-words minus one).

4. **DMA trigger constant:** `FA 20 00 00 6B 00 40 42 0F 00 00 C0` —
   identical to Google's binary, no per-section variation.

5. **CRC verification is internal to the chip.** Host does not compute
   CRCs. Reg `0x20000078`, bits 0/1 = code/config CRC bad. Read after
   reset to know whether firmware is valid.

If anyone wants to port this protocol into the kernel driver
(`request_firmware`-based loading), this implementation is the working
reference.
