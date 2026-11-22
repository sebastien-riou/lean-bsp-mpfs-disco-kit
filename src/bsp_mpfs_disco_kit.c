/* PolarFire SoC Discovery Kit: the lean-bsp contract on top of Microchip's mpfs_hal.
 *
 * One image serves all five harts. The HAL's startup code (platform/mpfs_hal/startup_gcc) runs on
 * the E51 first: it sets up clocks, DDR and I/O, wakes harts 1 to MPFS_HAL_LAST_HART, then calls
 * e51(); a woken U54 calls u54_<n>(). The application runs on hart BSP_APP_HART, which bsp_cli.py
 * sets from the target, along with MPFS_HAL_LAST_HART:
 *   target e51: BSP_APP_HART 0, e51() below is the application's; no other hart is woken.
 *   target u54: BSP_APP_HART 1, u54_1() below is the application's; e51() below is the companion
 *               code: it tells hart 1 that the E51 is done with the board, and parks.
 * Harts that are not woken wait in the startup code, interrupts off.
 *
 * The HAL's trap handler stops silently on an exception and re-arms its tick with a drift; the
 * image is linked with --wrap=trap_from_machine_mode so that exceptions and the timer interrupt
 * come here first, the vendor code staying as it is. init_memory() below takes the place of the
 * HAL's weak one. */
#include <errno.h>
#include <sys/stat.h>
#include "mpfs_hal/mss_hal.h"
#include "drivers/mss/mss_mmuart/mss_uart.h"
#include "drivers/mss/mss_gpio/mss_gpio.h"
#include "bsp_api.h"

#if !defined(BSP_APP_HART) || (BSP_APP_HART != 0 && BSP_APP_HART != 1)
#error "BSP_APP_HART must be 0 (the E51) or 1 (the first U54): bsp_cli.py sets it from the target"
#endif

#define MTIME_HZ      ((uint64_t)LIBERO_SETTING_MSS_RTC_TOGGLE_CLK) /* the CLINT's mtime: 1 MHz */
#define MTIME_PER_MS  (MTIME_HZ / 1000u)
#define LED_PIN       MSS_GPIO_9                                    /* on MSS GPIO1: GPIO_1_9 */

/* MMUART4 is the one on the FlashPro5's third interface (-if02) */
static mss_uart_instance_t *const uarts[] = {&g_mss_uart4_lo, &g_mss_uart1_lo};
#define N_UARTS (sizeof uarts / sizeof uarts[0])

static const bsp_capabilities_t capabilities = {
    .board_name = "mpfs-disco-kit",
    .n_uarts = N_UARTS,
    .n_leds = 1,
    .n_buttons = 0,
    .has_cycle_counter = true,
    .n_nvm_regions = 0,
    .nvm_regions = NULL,
};

const bsp_capabilities_t *bsp_get_capabilities(void) { return &capabilities; }

/* ---- the application hooks' defaults ---- */

__attribute__((weak)) void app_init(void) {}

__attribute__((weak)) void app_tick(uint32_t tick_ms) { (void)tick_ms; }

__attribute__((weak)) void app_on_fault(uint32_t reason, uintptr_t pc) {
    (void)reason;
    (void)pc;
}

__attribute__((weak)) void app_on_exit(int code) { (void)code; } /* the hart then parks */

/* ---- UART 0: MMUART4, UART 1: MMUART1; stdin and stdout are UART 0 ---- */

void bsp_uart_tx(unsigned port, const void *buf, size_t len) {
    if (port >= N_UARTS) return;
    const uint8_t *p = buf;
    while (len) { /* bounded by the baud rate */
        uint32_t n = len > 0x10000u ? 0x10000u : (uint32_t)len;
        MSS_UART_polled_tx(uarts[port], p, n);
        p += n;
        len -= n;
    }
}

/* Polled: what arrives while nobody calls this waits in the UART's 16-byte FIFO, and is lost beyond. */
size_t bsp_uart_rx(unsigned port, void *buf, size_t len) {
    if (port >= N_UARTS) return 0;
    return MSS_UART_get_rx(uarts[port], buf, len);
}

/* ---- LED and buttons ---- */

void bsp_led_set(unsigned id, bool on) {
    if (id == 0) MSS_GPIO_set_output(GPIO1_LO, LED_PIN, on ? 1u : 0u);
}

bool bsp_button_get(unsigned id) {
    (void)id;
    return false; /* no button offered */
}

/* ---- time: mcycle at the CPU clock, and the CLINT's mtime ---- */

uint32_t bsp_cycle_count(void) { return (uint32_t)read_csr(mcycle); }

uint32_t bsp_cpu_freq_hz(void) { return LIBERO_SETTING_MSS_COREPLEX_CPU_CLK; }

/* On mtime: works with interrupts masked, and is bounded. */
void bsp_delay_ms(uint32_t ms) {
    const uint64_t start = readmtime();
    const uint64_t ticks = (uint64_t)ms * MTIME_PER_MS;
    while (readmtime() - start < ticks) {
    }
}

/* A reset of the whole MSS, all harts included: the firmware starts over from the E51, as at power
 * up. Output still in a UART goes out first, for at most 10 ms. */
void bsp_reset(void) {
    const uint64_t start = readmtime();
    for (size_t i = 0; i < N_UARTS; i++) {
        while (!(uarts[i]->hw_reg->LSR & MSS_UART_TEMT) && readmtime() - start < 10u * MTIME_PER_MS) {
        }
    }
    SYSREG->MSS_RESET_CR = 0xDEADu;
    for (;;) {
    }
}

/* The instruction cache. The E51 has no other; nothing invalidates a U54's data cache, which is
 * why the u54 target does not offer the cache_clear primitive. */
void bsp_clear_caches(void) { __asm volatile("fence.i"); }

/* ---- the tick: mtimecmp of the application's hart, every millisecond ---- */

static uint64_t next_tick; /* in mtime */
static uint32_t tick_ms;

static void tick_start(void) {
    next_tick = readmtime() + MTIME_PER_MS;
    CLINT->MTIMECMP[BSP_APP_HART] = next_tick;
    set_csr(mie, MIP_MTIP);
}

/* Ticks that could not be taken in time (interrupts masked for long, a debugger halt) are not
 * replayed one by one: tick_ms jumps over them, so that it stays the time since the first tick. */
static __attribute__((noinline)) void tick(void) {
    const uint64_t missed = (readmtime() - next_tick) / MTIME_PER_MS;
    next_tick += (missed + 1u) * MTIME_PER_MS;
    CLINT->MTIMECMP[BSP_APP_HART] = next_tick; /* which also withdraws the interrupt */
    tick_ms += (uint32_t)missed + 1u;
    app_tick(tick_ms);
}

/* ---- traps: the linker routes the HAL's trap entry here (--wrap) ----
 * In eNVM like the trap entry, whose jump cannot reach the scratchpad the rest runs from. */

void __real_trap_from_machine_mode(uintptr_t *regs, uintptr_t dummy, uintptr_t mepc);

__attribute__((section(".bsp_envm_text"))) void __wrap_trap_from_machine_mode(uintptr_t *regs, uintptr_t dummy, uintptr_t mepc) {
    const uintptr_t cause = read_csr(mcause);
    if (read_csr(mhartid) != BSP_APP_HART) { /* no application hook on another hart */
        __real_trap_from_machine_mode(regs, dummy, mepc);
        return;
    }
    if ((cause & MCAUSE_INT) == 0) {
        app_on_fault(APP_FAULT_HARD, mepc);
        for (;;) { /* the terminal action: the hart stops here, interrupts off */
        }
    }
    if ((cause & MCAUSE_CAUSE) == IRQ_M_TIMER) {
        tick();
    } else {
        __real_trap_from_machine_mode(regs, dummy, mepc);
    }
}

/* ---- the copy to the scratchpad: the HAL's init_memory(), which is weak, plus the fence.i ----
 * The E51 runs this at each start, from eNVM, then goes on in the scratchpad. A hart must flush its
 * instruction cache before it executes what it wrote; the HAL does so for the U54s, not for the E51.
 * Seen on the board without it: a reset does not empty the E51's instruction cache, which can hold
 * blocks of the scratchpad fetched while it was being rebuilt. After bsp_reset() the E51 then
 * faulted on its own code (illegal instruction at the first instruction of a function), or did not
 * get as far as the application: every second bsp_reset() left the board silent. */

__attribute__((section(".bsp_envm_text"))) void init_memory(void) {
    copy_section(&__text_load, &__text_start, &__text_end);
    copy_section(&__sdata_load, &__sdata_start, &__sdata_end);
    copy_section(&__data_load, &__data_start, &__data_end);
    zero_section(&__sbss_start, &__sbss_end);
    zero_section(&__bss_start, &__bss_end);
    __asm volatile("fence.i");
    __disable_all_irqs();
}

/* ---- newlib's system calls: the heap, and UART 0 as stdin and stdout ---- */

extern char __heap_start; /* linker/default.ld */
extern char __heap_end;

/* newlib's heap only grows, through _sbrk, so how far its end moved since bsp_heap_usage_reset()
 * is how much more heap the code run since then needed. */
static char *heap_now = &__heap_start;
static char *heap_mark = &__heap_start;
static char *heap_peak = &__heap_start;

void bsp_heap_usage_reset(void) {
    heap_mark = heap_now;
    heap_peak = heap_now;
}

size_t bsp_heap_usage(void) { return heap_peak > heap_mark ? (size_t)(heap_peak - heap_mark) : 0; }

void *_sbrk(ptrdiff_t incr) {
    if (incr > &__heap_end - heap_now || incr < &__heap_start - heap_now) {
        errno = ENOMEM;
        return (void *)-1;
    }
    char *previous = heap_now;
    heap_now += incr;
    if (heap_now > heap_peak) heap_peak = heap_now;
    return previous;
}

int _write(int file, char *ptr, int len) {
    (void)file;
    if (len > 0) bsp_uart_tx(0, ptr, (size_t)len);
    return len;
}

int _read(int file, char *ptr, int len) {
    (void)file;
    if (len <= 0) return 0;
    size_t n;
    while ((n = bsp_uart_rx(0, ptr, (size_t)len)) == 0) {
    }
    return (int)n;
}

int _close(int file) {
    (void)file;
    return -1;
}

int _fstat(int file, struct stat *st) {
    (void)file;
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int file) {
    (void)file;
    return 1;
}

off_t _lseek(int file, off_t offset, int whence) {
    (void)file;
    (void)offset;
    (void)whence;
    return 0;
}

int _getpid(void) { return 1; }

int _kill(int pid, int sig) {
    (void)pid;
    (void)sig;
    errno = EINVAL;
    return -1;
}

void _exit(int code) {
    (void)code;
    __disable_irq();
    for (;;) {
    }
}

/* ---- the companion code: what the E51 runs when the application is on a U54 ----
 * The E51 wakes hart 1 before it is done itself: it goes on switching clocks and resets of MSS
 * blocks (the FICs, the fabric) for a while, then calls e51(). Those are read-modify-writes of the
 * registers through which hart 1 switches on its UARTs and GPIO; done from both harts at once, one
 * hart's update is lost (seen on the board: a UART left in reset, a FIC left without clock). So
 * hart 1 touches nothing until the E51 says it is done. */
#if BSP_APP_HART != 0
static volatile uint32_t e51_done;

void e51(void) {
    clear_csr(mstatus, MSTATUS_MIE);
    write_csr(mie, 0);
    e51_done = 1;
    for (;;) { /* parked: nothing wakes it */
        __asm volatile("wfi");
    }
}
#endif

/* ---- the application's hart ---- */

static void run_application(void) {
#if BSP_APP_HART != 0
    while (!e51_done) { /* see above; the E51 gets there within its startup code */
    }
#endif
    clear_soft_interrupt(); /* the E51's wake-up call, still pending on a U54 */
#ifdef __riscv_flen
    set_csr(mstatus, MSTATUS_FS); /* the FPU, whatever the MSS configuration says of it */
#endif
    (void)mss_config_clk_rst(MSS_PERIPH_MMUART1, BSP_APP_HART, PERIPHERAL_ON);
    (void)mss_config_clk_rst(MSS_PERIPH_MMUART4, BSP_APP_HART, PERIPHERAL_ON);
    (void)mss_config_clk_rst(MSS_PERIPH_GPIO1, BSP_APP_HART, PERIPHERAL_ON);
    for (size_t i = 0; i < N_UARTS; i++) {
        MSS_UART_init(uarts[i], MSS_UART_115200_BAUD, MSS_UART_DATA_8_BITS | MSS_UART_NO_PARITY | MSS_UART_ONE_STOP_BIT);
    }
    MSS_GPIO_init(GPIO1_LO);
    MSS_GPIO_config(GPIO1_LO, LED_PIN, MSS_GPIO_OUTPUT_MODE);
    MSS_GPIO_set_output(GPIO1_LO, LED_PIN, 0u);
    bsp_heap_usage_reset();

    app_init(); /* interrupts still masked */
    tick_start();
    __enable_irq();
    int rc = app_main();
    app_on_exit(rc);
    for (;;) { /* app_main returned: park here rather than starting over (lean-bsp contract) */
        __asm volatile("wfi");
    }
}

#if BSP_APP_HART == 0
void e51(void) { run_application(); }
#else
void u54_1(void) { run_application(); }
#endif
