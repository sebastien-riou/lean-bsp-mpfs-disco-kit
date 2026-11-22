#!/usr/bin/env python3
"""lean-bsp entry point of the PolarFire SoC Discovery Kit BSP, made of lean-bsp's board helpers
(leanbsp.backends).

build makes one image for all the harts, as Microchip's HAL (platform/) is built: its startup code
and drivers, src/ and the application, compiled with the riscv-none-elf toolchain named by
CROSS_COMPILE (default: riscv-none-elf- from PATH) and the CPU flags of the target, the hart the
application runs on. The headers describing the MSS configuration are generated into the build
directory from the XML in board/. The image boots from eNVM (linker/default.ld, or the
application's own script). load and run go through lean-hwbridge; the board is the usbman nickname
in HWBRIDGE_BOARD, default mpfs-disco-kit.
"""

import json
import subprocess
import sys
from pathlib import Path

try:
    from leanbsp.backends import board, gcc
except ImportError:
    print(json.dumps({'status': 'error', 'message': 'this BSP needs a lean-bsp with leanbsp.backends: update lean-bsp'}))
    sys.exit(1)

ROOT = Path(__file__).resolve().parent
PLATFORM = ROOT / 'platform'
HAL = PLATFORM / 'mpfs_hal'
MSS_DESCRIPTION = ROOT / 'board' / 'fpga_design' / 'design_description'  # the XML Libero's MSS configurator writes
GENERATOR = PLATFORM / 'soc_config_generator' / 'mpfs_configuration_generator.py'

# The hart the application runs on. The E51 wakes the harts up to that one, and no other.
APP_HART = {'e51': 0, 'u54': 1}

# mss_entry.S comes first: the linker script puts every .text.init section, in link order, at the
# start of the image, and that file's holds the reset and trap vectors, made of 4-byte instructions
# only. Behind another file's, the trap vector can land on an address mtvec cannot hold, and every
# hart then loops forever at reset. build() checks the result.
ENTRY = HAL / 'startup_gcc' / 'mss_entry.S'
SOURCES = [
    ENTRY,
    PLATFORM / 'hal' / 'hw_reg_access.S',
    HAL / 'startup_gcc' / 'mss_utils.S',
    *sorted((PLATFORM / 'hal').glob('*.c')),
    *sorted((HAL / 'common').glob('*.c')),
    *sorted((HAL / 'common' / 'nwc').glob('*.c')),
    HAL / 'startup_gcc' / 'system_startup.c',  # not newlib_stubs.c: src/ has the system calls, on UART 0
    PLATFORM / 'drivers' / 'mss' / 'mss_mmuart' / 'mss_uart.c',
    PLATFORM / 'drivers' / 'mss' / 'mss_gpio' / 'mss_gpio.c',
    *sorted((ROOT / 'src').glob('*.c')),
]
INCLUDE_DIRS = [ROOT / 'include', PLATFORM, ROOT / 'board']
# As Microchip's own build of this code (SoftConsole's release configuration). Its trap entry reads
# mbadaddr, a name of mtval that today's assemblers no longer know.
BOARD_FLAGS = ['-std=gnu11', '-fsigned-char', '-Dmbadaddr=mtval']
OPTIMISATION = ['-Os', '-g']  # for size: the eNVM holds 128 KiB


def mss_headers(bdir: Path) -> Path:
    """fpga_design_config/, the headers the HAL takes the MSS configuration from, generated in the
    build directory by Microchip's generator; returns the directory to add to the include path."""
    out = bdir / 'mss'
    done_marker = out / 'fpga_design_config' / 'fpga_design_config.h'
    descriptions = sorted(MSS_DESCRIPTION.glob('*.xml'))
    if len(descriptions) != 1:
        raise board.BuildError(f'{MSS_DESCRIPTION} must hold exactly one MSS description (XML), found {len(descriptions)}')
    if done_marker.exists() and done_marker.stat().st_mtime >= descriptions[0].stat().st_mtime:
        return out
    out.mkdir(parents=True, exist_ok=True)
    cmd = [sys.executable, str(GENERATOR), f'{MSS_DESCRIPTION}/', str(out)]
    print(f'generating the MSS configuration headers from {descriptions[0].relative_to(ROOT)}', flush=True)
    done = subprocess.run(cmd, capture_output=True, text=True)
    if done.returncode != 0 or not done_marker.exists():
        sys.stdout.write(done.stdout + done.stderr)
        raise board.BuildError('generating the MSS configuration headers failed')
    return out


def check_trap_vector(prefix: str, elf: Path) -> None:
    symbols = subprocess.run([f'{prefix}nm', str(elf)], capture_output=True, text=True, check=True).stdout
    address = next((int(line.split()[0], 16) for line in symbols.splitlines() if line.split()[-1] == 'trap_vector'), None)
    if address is None or address % 4:
        elf.unlink()
        where = 'is missing' if address is None else f'is at 0x{address:x}, which mtvec cannot hold'
        raise board.BuildError(f'trap_vector {where}: mss_entry.S must be linked first')


def build(app: dict, target, bdir: Path) -> dict:
    hart = APP_HART[target.name]
    defines = ['NDEBUG', f'BSP_APP_HART={hart}', 'MPFS_HAL_FIRST_HART=0', f'MPFS_HAL_LAST_HART={hart}']
    # Faults and the tick go through the BSP before the HAL's trap handler: see src/.
    link_flags = ['-nostartfiles', '-Wl,--wrap=trap_from_machine_mode', f'-L{ROOT / "linker"}']
    artifact = gcc.build(app, target, bdir, root=ROOT, sources=SOURCES, include_dirs=[*INCLUDE_DIRS, mss_headers(bdir)],
                         defines=defines, linker_script=ROOT / 'linker' / 'default.ld', link_flags=link_flags,
                         optimisation=OPTIMISATION, board_flags=BOARD_FLAGS)
    check_trap_vector(gcc.cross_prefix(target), Path(artifact['elf']))
    return artifact


BOARD = board.Board(
    root=ROOT,
    title='PolarFire SoC Discovery Kit',
    flash_by_hand='java -jar $SC_INSTALL_DIR/extras/mpfs/mpfsBootmodeProgrammer.jar --bootmode 1 --die MPFS095T '
                  '--package FCSG325 --workdir <the directory of the ELF> {elf} (SoftConsole\'s boot mode programmer, '
                  'with SC_INSTALL_DIR and FPGENPROG set)',
    run_by_hand='Power-cycle the board and read the FlashPro5\'s serial ports at 115200 baud',
)

if __name__ == '__main__':
    sys.exit(board.main(BOARD, build))
