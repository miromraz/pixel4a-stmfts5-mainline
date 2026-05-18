# Pixel 4a (sunfish) FTM5 touchscreen — mainline enablement

Patches and tooling to bring up the STMicroelectronics FTM5CU56AA1BE
touchscreen (chip ID 0x4836) on the Google Pixel 4a (sunfish) running
mainline Linux. Tested on kernel 7.1-rc3 (sm7150-mainline) under
postmarketOS — device boots and the user can navigate the UI by touch
while battery + charging work concurrently.

## What's here

### `patches/` — kernel patch series (v3)

Applies on top of David Heidelberg's stmfts5 v4 series (Message-Id:
`<20260409-stmfts5-v4-0-64fe62027db5@ixit.cz>`,
[lore link](https://lore.kernel.org/lkml/20260409-stmfts5-v4-0-64fe62027db5@ixit.cz/)).

- **0001** — `Input: stmfts - poll for CONTROLLER_READY event in stmfts5_configure`
  Replaces the single I2C read after reset with a poll loop (25 ms
  intervals, ~500 ms cap). FTM5 firmware boot includes internal CRC
  verification and can take several hundred ms before the chip's I2C
  state machine ACKs; the single-read pattern races the firmware and
  loses, returning `-ENXIO` silently. Polling exits the moment
  `CONTROLLER_READY` arrives — average-case zero added delay.

  Empirical aside in the commit body: the FTS5 chip only emits event
  data via `STMFTS_READ_ALL_EVENT` (0x86); `STMFTS_READ_ONE_EVENT`
  (0x85) returns nothing on the first post-reset read. v4 already uses
  0x86 here.

  Diagnosed via `ftrace function_graph` + `funcgraph-retval` (failing
  `__i2c_transfer` returned `-6` directly; `dmesg` surfaced no error of
  its own). Approach inspired by Samsung's downstream
  `ftm4_ts.c::fts_wait_for_ready` and map220v's downstream sm7125
  stmfts driver.

- **0002** — `arm64: dts: qcom: sm7150-sunfish: add STMicro FTM5 touchscreen`
  Adds the touchscreen node, pinctrl, and supplies. Non-obvious bits:
  - `vdd` and `avdd` are **separate physical supplies**, both shared
    with the OLED panel because touch and display are one bonded
    assembly. `vdd` (1.8 V digital) → `vreg_l13a_1p8`, `avdd` (3.0 V
    analog) → `vreg_l7c_3p0`. PM6150 GPIO 4 enables an external load
    switch upstream of these rails (asserted via pinctrl).
  - `mode-switch-gpios` uses `GPIO_ACTIVE_LOW`. Empirically, driving
    the line HIGH puts the chip into SLPI-routed mode where the
    AP-side I2C address goes silent. LOW keeps it on the AP path.

### `userspace-flasher/` — bringup tool

`ftm5_flash.c` is a self-contained static aarch64 userspace flasher
that implements the full FTM5 firmware update protocol (bootloader
entry, page-by-page erase, DMA fill, post-flash boot). Built from
reverse-engineering Google's downstream `ftm5.ko` and cross-checked
against
[kerneltoast/floral fts_lib/ftsFlash.c](https://github.com/kerneltoast/android_kernel_google_floral/blob/11.0.0-sultan/drivers/input/touchscreen/fts_touch/fts_lib/ftsFlash.c).

Not part of the patch series — included as reference for the FTM5 wire
protocol if anyone wants to add `request_firmware`-based loading to
the kernel driver in a future round. See `userspace-flasher/README.md`
for how to build/use.

## Version history

- **v1**: `msleep(500)` in `stmfts_reset`. David's feedback: fixed
  sleep is wasteful and the downstream driver reads chip state during
  the wait instead of blocking. Same v1 DTS used a single
  `vreg_touch_en` fixed regulator for both rails. David's feedback:
  `vreg_touch_en` is not both digital `vdd` and analog `avdd` — these
  are physically separate rails.
- **v2**: poll loop in `stmfts5_configure`; DTS unchanged.
- **v3** *(current)*: incorporates both pieces of v1 feedback. Poll
  loop preserved from v2; DTS split into proper `vdd`/`avdd` supplies
  and moves the load-switch enable to pinctrl on PM6150 GPIO 4.

## End-to-end result

```
$ cat /proc/bus/input/devices | grep -A 4 stmfts
N: Name="stmfts"
P: Phys=
S: Sysfs=/devices/platform/soc@0/ac0000.geniqup/a84000.i2c/i2c-7/7-0049/input/input2
H: Handlers=event2
```

`/dev/input/event2` reports MT_B touch events with correct X/Y inside
the 1080x2340 panel range. UI navigation via touch verified live.

Concurrent charging state from the same kernel:
```
qcom_qg/capacity:   61%, Charging, +651 mA, 4.006 V
pm8150b-charger:    online=1, status=Charging
```

## Combined bringup branch

For a full working kernel that boots the device end-to-end (this
series + PM6150 charger/qgauge DT enablement), see:

  https://github.com/miromraz/linux/tree/sunfish-mainline

Branch base: `sm7150-mainline/linux` v7.1-rc3.

## Status

v3 ready to post to David Heidelberg, May 2026.
