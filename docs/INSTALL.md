# Installing HackiOS as a dual boot

The developer setup boots HackiOS from a computer: `fastboot boot` puts the UEFI in RAM, `xl` sends the kernel and
the system image over USB, a session lasts as long as `XL_SECS`. An **installed** phone needs no computer: you
switch it on, a menu appears, and HackiOS runs until you switch it off -- next to an Android that is never touched.

## How it is laid out

| Where | What |
|---|---|
| Android's A/B slot (e.g. `_b`) | Android, as it was. Nothing of it is written. |
| the spare slot's `boot` (e.g. `boot_a`) | the UEFI (Project Silicium port) with our loader, no payload in it |
| `darwin`, blocks 0..2042367 | the iOS volume and both cryptexes (written once, see the userland README) |
| `darwin`, blocks 2042368.. | **boot area**: two bundle slots of 96 MiB -- kernel, boot volume, command line |
| `darwin`, blocks 2097152.. | the var store (what iOS writes survives a reboot), two slots |

At power-on the phone's bootloader starts the active slot. When that is the spare slot, the UEFI starts our loader
with no payload, and the loader shows its menu:

```
  HACKiOS boot menu        volume: move   power: select
   > HackiOS
     Android
     Developer mode (USB, xl)
  HackiOS in 5 s
```

* **HackiOS** -- the newest good bundle from darwin's boot area (header and contents checked against their sums;
  a torn one falls back to the other slot) runs with no time limit.
* **Android** -- the loader exchanges the A/B bytes of `boot_a` and `boot_b` in the GPT (backup table first, then
  the primary one, both read back) and resets: the phone comes up in Android.
* **Developer mode** -- the USB link `xl` uses, as after `xl enter`.

## Install

Needs: an unlocked bootloader, Android with root (Magisk) and adb, the `darwin` partition with the iOS volume and
the cryptexes on it, the workspace built (`common/setup.sh`, `ios165/fetch.sh`).

```sh
common/flash/install.sh redwood
```

It builds the boot volume and the bundle, shows the state (`flashctl.py status`), backs up `boot`, `vbmeta`, `dtbo`,
`vendor_boot` of both slots and the GPT of their unit into `backups/<serial>/<time>/`, writes the bundle into
darwin and the UEFI into the spare slot's `boot`, and reads every write back. Then:

```sh
python3 common/flash/flashctl.py switch hackios      # from Android: bootctl set-active-boot-slot, reboot
```

## Update

A new kernel or boot volume goes into the boot slot that does not hold the running bundle:

```sh
common/flash/install.sh redwood update
```

If that write is cut off (cable, battery), the phone still boots the previous bundle.

## Getting Android back -- three ways, from the easiest

1. The HackiOS menu: **Android**.
2. Fastboot (volume down + power): `python3 common/flash/flashctl.py switch android`, or by hand
   `fastboot --set-active=b && fastboot reboot` (the slot Android was on; the backup's `manifest.json` says which).
3. Remove the UEFI: `python3 common/flash/flashctl.py restore` puts the spare `boot` back from the backup.

Fastboot lives in the phone's own bootloader, which this install never writes: whatever happens to the UEFI or
the loader, the phone can be brought back with a cable.

## Things to know

* **Android updates (OTA)** write the spare slot -- they overwrite the UEFI, and for a while after an update the
  slots must not be switched (`flashctl.py switch` refuses while a Virtual A/B merge is pending). After an update:
  `common/flash/install.sh <device>` again.
* **Do not lock the bootloader** with HackiOS installed.
* The menu is a text console for now; the loader's own graphics are a later step.
* Tested: the whole boot path in QEMU on a disk laid out like the phone (`common/flash/qemu-test.sh auto` boots the
  bundle from darwin; `qemu-test.sh android` switches the slots and leaves both GPTs sound). The first install on a
  real phone should be done with the backup at hand.
