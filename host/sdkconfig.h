// sdkconfig.h stub for the host build, real one comes from ESP-IDF menuconfig.
// Override individual options with -D on the compiler command line.

#ifndef CONFIG_EMU_RAM_MB
#define CONFIG_EMU_RAM_MB 16
#endif

#ifndef CONFIG_EMU_CONSOLE_UART0
#define CONFIG_EMU_CONSOLE_UART0 1
#endif

#ifndef CONFIG_EMU_CONSOLE_USB_JTAG
#define CONFIG_EMU_CONSOLE_USB_JTAG 1
#endif
