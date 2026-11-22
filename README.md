# lean-bsp-mpfs-disco-kit

The [lean-bsp](https://github.com/sebastien-riou/lean-bsp) BSP for Microchip's PolarFire SoC
Discovery Kit (MPFS095T: one E51 and four U54 64-bit RISC-V harts at 600 MHz). Any lean-bsp
application builds and runs on either kind of hart:

````
lbsp build-load-run --bsp mpfs-disco-kit --target e51 --until 'hello-world done' --timeout 120
lbsp build-load-run --bsp mpfs-disco-kit --target u54 --until 'hello-world done' --timeout 120
````

**Status: runs on the kit, on both targets.** Checked through lean-hwbridge: hello-world's full
report after a flash and after a reset, UART reception, the tick, the fault hook, and `bsp_reset()`
in a loop (14 restarts in a row on `e51`, 12 on `u54`). Not checked: the LED by eye, and UART 1.

## One application hart, the others the BSP's

| Target | The application runs on | Compiled for | The other harts |
|---|---|---|---|
| `e51` | hart 0, the E51 | rv64imac, lp64 | never started |
| `u54` | hart 1, the first U54 | rv64gc, lp64d | the E51 sets the board up, wakes hart 1 and parks; harts 2 to 4 are never started |

The E51 always boots first: it is the one that sets up clocks, DDR and I/O, and the one that wakes
a U54. For `u54` that part is the BSP's own, in the same image as the application: lean-bsp calls it
companion code. It has no application hooks and prints nothing. UART 0, the LED, the tick and
`bsp_reset()` belong to the application's hart; `bsp_reset()` restarts all of them.

The E51 wakes hart 1 before it has finished with the MSS's clock and reset registers, the same
registers a hart switches its peripherals on through. Hart 1 therefore waits for the E51 to say it
is done before touching anything: without that, updates of one hart were lost on the board (a UART
left in reset, a FIC left without clock).

For `u54` the whole image is compiled for the U54, the startup code the E51 executes included.
That is how Microchip builds its own examples: the code on the E51's path has one floating-point
instruction, which the HAL skips on hart 0.

## What it needs

- A riscv-none-elf toolchain with the `rv64imac/lp64` and `rv64gc/lp64d` libraries, such as
  xPack's 15.2.0. `CROSS_COMPILE` selects it, e.g.
  `CROSS_COMPILE=/opt/toolchains/riscv-none-elf-gcc/xpack-riscv-none-elf-gcc-15.2.0-1/bin/riscv-none-elf-`;
  without it, `riscv-none-elf-gcc` is taken from `PATH`.
- For `load` and `run`, [lean-hwbridge](https://github.com/sebastien-riou/lean-hwbridge) serving the
  board with its `mpfs` profile: `hwbridge` on `PATH`, or the command in `HWBRIDGE`. The board is
  its usbman nickname, `HWBRIDGE_BOARD`, by default `mpfs-disco-kit`. The bridge's machine needs
  Microchip's SoftConsole (the boot mode programmer and OpenOCD) and Libero's `fpgenprog`. Without
  a bridge, `load` prints the programmer's command line.
- An FPGA design whose MSS configuration is the one in `board/` (below). The BSP uses nothing of
  the fabric.

**`load` replaces what the eNVM holds.** On a kit as shipped that is the Hart Software Services,
the boot loader that starts Linux; programming the kit's reference design again brings it back.

### The MSS configuration

`board/fpga_design/design_description/` holds the XML that Libero's MSS configurator writes; the
build turns it into the headers the HAL takes clocks, DDR, I/O and memory settings from
(`<build dir>/mss/fpga_design_config/`). The one here, `MPFS_DISCOVERY_KIT_MSS.xml`, is the
Discovery Kit's in Microchip's `mpfs-blank-baremetal` example: the MSS configuration of the kit's
[reference design](https://github.com/polarfire-soc/polarfire-soc-discovery-kit-reference-design).
`board/fpga_design/mss_configuration/` has the configurator's input it was made from. What the BSP
relies on in it: the 600 MHz CPU clock, the 1 MHz `mtime`, and MMUART1, MMUART4 and GPIO1 bit 9 on
the MSS's own pins.

For another MSS configuration, replace the XML: the next build regenerates the headers.

## How the contract maps to the board

| Primitive | On the Discovery Kit |
|---|---|
| UART 0 (and `printf`, `stdin`) | MMUART4, 115200 8N1: the second serial port of the on-board FlashPro5 (FT4232H), `/dev/serial/by-id/...FlashPro5...-if02-port0`; bytes as they are: no `\r` added before `\n` |
| UART 1 | MMUART1, which Microchip's examples print on: the FlashPro5's first serial port |
| LEDs | 0 is the LED on MSS GPIO1 bit 9 |
| Buttons | none offered |
| Cycle counter, CPU frequency | `mcycle`, at 600 MHz |
| Delay | busy wait on `mtime` (1 MHz): works with interrupts masked |
| Cache clear | `fence.i`, the instruction cache. That is every cache the E51 has; a U54's data cache cannot be invalidated, so `u54` does not offer `cache_clear` |
| Cycle timer | not offered |
| Heap usage | growth of the heap end since the last reset, tracked in `_sbrk` |
| Reset | a reset of the whole MSS (`MSS_RESET_CR`), once the UARTs sent the pending output |
| `app_tick` | from the application hart's timer interrupt, every millisecond of `mtime`. Ticks that cannot be taken in time are skipped, and `tick_ms` jumps over them |
| `app_on_fault` | from the trap handler on any exception, `pc` is `mepc`; the hart then stops |
| `app_on_exit` | default does nothing: the hart then parks in `wfi`, the tick still running |
| Flash/NVM | not yet |

### Memory

The image is stored in eNVM and, but for its startup code, copies itself to an L2 cache scratchpad
and runs from there, as Microchip's `envm-scratchpad` configuration does.

| | Size | Holds |
|---|---|---|
| eNVM | 128 KiB less 256 bytes | code and initialised data. hello-world takes 35 KiB of it, the HAL's DDR training included |
| Scratchpad | 512 KiB | the running code, data, a 128 KiB heap and a 64 KiB stack for the application |

An application too large for the eNVM does not link. The HAL sets up and trains the DDR at every
boot; the BSP places nothing there.

### Reset

The firmware prints its first line 5 to 10 s after a reset: the HAL's hardware setup, DDR training
included, at every boot.

`bsp_reset()` writes the MSS's reset register: every hart starts over, as after lean-hwbridge's
`reset`, which goes through the debugger. Neither clears everything: the E51's instruction cache
and the CLINT's `mtimecmp` registers keep what they held. The BSP's startup deals with both (the
first is why it has its own `init_memory()`, below).

### Interrupts and floating point

The HAL's trap entry saves the integer registers only. On `u54`, code run from an interrupt
(`app_tick`) must therefore not use floating point.

## Layout

- `platform/`: from Microchip's [platform](https://github.com/polarfire-soc/platform), release
  2026.08 (commit 8d52d59), the directories the BSP builds, as they are: the HAL (`hal/`, `mpfs_hal/`
  2.3.110) with its startup code, the MMUART and GPIO drivers and the configuration generator. MIT
  licence (`platform/LICENSE.md`).
- `board/`: the Discovery Kit's files of the `mpfs-blank-baremetal` application in Microchip's
  [bare metal examples](https://github.com/polarfire-soc/polarfire-soc-bare-metal-examples), release
  2026.10 (commit d1b3604), as they are: the MSS description (above) and
  `mpfs_hal_config/mss_sw_config.h`, the HAL's software settings of its `envm-scratchpad-release`
  configuration. The build sets the harts to start on top of it.
- `linker/default.ld`: the linker script of that same configuration, adapted (its header says how).
  An application needing its own script sets `linker_script` in `app.toml`.
- `src/bsp_mpfs_disco_kit.c`: the application's hart (`e51()` or `u54_1()`), the companion code
  (`e51()` when the application is on a U54), the primitives, the default application hooks, the
  tick, the fault hook, `init_memory()` and newlib's system calls.
- `include/bsp_api.h`: the contract header, identical to lean-bsp's canonical copy (`lbsp check`
  verifies it).
- `bsp.toml`, `bsp_cli.py`: what lean-bsp reads and runs. `bsp_cli.py` is made of lean-bsp's board
  helpers (`leanbsp.backends`), which lean-bsp runs it with.

Four things let the vendor code stay untouched:

- its `newlib_stubs.c` is left out of the build: `src/` has the system calls, on UART 0;
- the image is linked with `--wrap=trap_from_machine_mode`, so that exceptions and the timer
  interrupt reach the BSP before the HAL's trap handler, which stops silently on an exception;
- `mbadaddr`, which its trap entry reads, is defined as `mtval`, the name today's assemblers know;
- `init_memory()`, which the HAL defines weak, is the BSP's: the same copy of the image from eNVM to
  the scratchpad, then a `fence.i`. The HAL's leaves the E51 with an instruction cache that can
  hold stale blocks of the scratchpad, since a reset does not empty it. On the kit that made every
  second `bsp_reset()` on `e51` end in an illegal instruction on valid code, or in silence.
