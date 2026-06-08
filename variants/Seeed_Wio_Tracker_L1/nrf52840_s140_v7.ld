/* Linker script to configure memory regions — S140 v7.x (7.3.0) app layout.
 *
 * Vendored for the Seeed Wio Tracker L1 Pro: the unit ships with the Adafruit
 * nRF52 UF2 bootloader carrying SoftDevice S140 *7.3.0* (confirmed on the device
 * via INFO_UF2.TXT). S140 7.x occupies 0x27000 of flash, so the application code
 * base is 0x27000 — 0x1000 higher than S140 6.1.1's 0x26000. The stock Adafruit
 * core in this PlatformIO framework only ships nrf52840_s140_v6.ld (app @
 * 0x26000); linking against that would place the app inside the last 4 KB of the
 * 7.3.0 SoftDevice and it would not boot. This is a verbatim copy of the v6
 * script with the single required change: FLASH ORIGIN 0x26000 -> 0x27000 (and
 * the matching LENGTH expression). RAM layout is unchanged (the core reserves a
 * generous 0x20006000 app-RAM base that is valid for both SD 6.x and 7.x).
 *
 * Selected via [env:wio_tracker_l1] board_build.ldscript. nrf52_common.ld is
 * still resolved from the framework linker dir (on LIBPATH).
 */

SEARCH_DIR(.)
GROUP(-lgcc -lc -lnosys)

MEMORY
{
  FLASH (rx)     : ORIGIN = 0x27000, LENGTH = 0xED000 - 0x27000

  /* SRAM required by Softdevice depend on
   * - Attribute Table Size (Number of Services and Characteristics)
   * - Vendor UUID count
   * - Max ATT MTU
   * - Concurrent connection peripheral + central + secure links
   * - Event Len, HVN queue, Write CMD queue
   */
  RAM (rwx) :  ORIGIN = 0x20006000, LENGTH = 0x20040000 - 0x20006000
}

SECTIONS
{
  . = ALIGN(4);
  .svc_data :
  {
    PROVIDE(__start_svc_data = .);
    KEEP(*(.svc_data))
    PROVIDE(__stop_svc_data = .);
  } > RAM

  .fs_data :
  {
    PROVIDE(__start_fs_data = .);
    KEEP(*(.fs_data))
    PROVIDE(__stop_fs_data = .);
  } > RAM
} INSERT AFTER .data;

INCLUDE "nrf52_common.ld"
