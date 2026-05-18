# Pixel 4a (sunfish) FTM5 touchscreen — mainline enablement

Patches and tooling to bring up the STMicroelectronics FTM5CU56AA1BE
touchscreen (chip ID 0x4836) on the Google Pixel 4a (sunfish) running
mainline Linux. Tested on kernel 7.1-rc3 (sm7150-mainline) under
postmarketOS.

## What's here

### `patches/` — kernel patch series (v2)

Applies on top of David Heidelberg's stmfts5 v4 series (Message-Id:
`<20260409-stmfts5-v4-0-64fe62027db5@ixit.cz>`,
[lore link](https://lore.kernel.org/lkml/20260409-stmfts5-v4-0-64fe62027db5@ixit.cz/)).

- **0001** — `Input: stmfts - poll for chip ready in stmfts5_configure`
  Replaces the single I2C read after reset with a poll loop (retry every
  25 ms, up to ~500 ms total). FTM5 firmware boot includes internal CRC
  verification and can take several hundred ms before the chip's I2C
  state machine begins ACKing — too long for the existing fixed 50 ms
  delay in `stmfts_reset`. Polling avoids penalising every probe with a
  worst-case sleep. Symptom diagnosed via `ftrace function_graph` with
  `funcgraph-retval` (failing `__i2c_transfer` returned `-6` directly;
  `dmesg` surfaced no error of its own). Approach inspired by map220v's
  downstream sm7125 driver.

- **0002** — `arm64: dts: qcom: sm7150-google-sunfish: add stmfts5 touchscreen`
  Adds the touchscreen node, vreg_touch_en (regulator-fixed on pm6150 GPIO 4
  modeling the downstream load-switch arrangement), and pinctrl states for
  IRQ + reset pins. Two non-obvious bits called out in the commit body:
  - `mode-switch-gpios` uses `GPIO_ACTIVE_LOW`. Empirically, driving the
    line HIGH puts the chip into SLPI-routed mode where the AP-side I2C
    address goes silent. LOW keeps it on the AP path.
  - Reset pin explicitly pinctrl'd as GPIO output high with pull-up so the
    chip is not held in reset between driver lifecycle stages.

### `userspace-flasher/` — bringup tool

`ftm5_flash.c` is a self-contained static aarch64 userspace flasher that
implements the full FTM5 firmware update protocol (bootloader entry,
page-by-page erase, DMA fill, post-flash boot). Built from
reverse-engineering Google's downstream `ftm5.ko` and cross-checked against
the GPL'd reference at
[kerneltoast/floral fts_lib/ftsFlash.c](https://github.com/kerneltoast/android_kernel_google_floral/blob/11.0.0-sultan/drivers/input/touchscreen/fts_touch/fts_lib/ftsFlash.c).

Not part of the patch series — included as reference for the FTM5 wire
protocol if anyone wants to add `request_firmware`-based loading to the
kernel driver in a future round. See `userspace-flasher/README.md` for
how to build/use.

## Version history

- **v1** (earlier commit): used `msleep(500)` in `stmfts_reset`. David
  pointed out the fixed sleep is wasteful and noted that the downstream
  driver (map220v/sm7125-mainline) reads chip state during the wait
  instead of blocking.
- **v2** (current): polling loop in `stmfts5_configure` — average-case
  zero added delay, worst case bounded at 500 ms.

## End-to-end result

After applying the patches and booting on Pixel 4a:

```
$ cat /proc/bus/input/devices | grep -A 4 stmfts
N: Name="stmfts"
P: Phys=
S: Sysfs=/devices/platform/soc@0/ac0000.geniqup/a84000.i2c/i2c-7/7-0049/input/input2
H: Handlers=event2
```

`/dev/input/event2` reports MT_B touch events with correct X/Y within
the 1080x2340 panel range.

## Status

v2 posted to David Heidelberg for review after his v1 feedback,
May 2026.
