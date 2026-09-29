/* ASC0 asynchronous serial channel.
 *
 * TX and RX advance as register-timed ASC frames.  TBUF is double-buffered:
 * the active byte remains in the shift register while one following byte may
 * wait in TBUF.  Asynchronous TIR is raised at the beginning of the stop-bit
 * interval and output becomes host-visible only when the complete frame has
 * left the shift register.  Host RX bytes are queued until S0CON.R and
 * S0CON.REN enable reception, then complete as frames and raise S0RIC.RIR.
 * The optional BSL autobaud bypass sets the 0xFF10 exit bit; P7.3 line-sense
 * toggling is owned by the ports peripheral. TX/RX byte buffers live on the SoC. */
#ifndef CEMU_PERIPH_SERIAL_H
#define CEMU_PERIPH_SERIAL_H

#include "peripheral.h"

typedef struct {
    int tx_active;
    int tx_tir_raised;
    uint8_t tx_byte;
    uint16_t tx_con;
    uint16_t tx_bg;
    uint16_t tx_fdv;
    unsigned tx_frame_bits;
    uint64_t tx_start_tick;
    uint64_t tx_tir_tick;
    uint64_t tx_completion_tick;
    int tx_buffer_full;
    uint8_t tx_buffer_byte;

    int rx_full;
    int rx_active;
    uint8_t rx_byte;
    uint16_t rx_con;
    uint16_t rx_bg;
    uint16_t rx_fdv;
    unsigned rx_frame_bits;
    uint64_t rx_start_tick;
    uint64_t rx_completion_tick;
} serial_state_t;

void cemu_serial_periph_init(peripheral_t *p, serial_state_t *st);

/* Start the first pending host byte if the live ASC configuration permits it.
 * cemu_soc_feed_serial() calls this after appending a batch; S0CON writes call it
 * when R/REN becomes usable. */
void cemu_serial_rx_kick(peripheral_t *p, soc_t *s);

#endif /* CEMU_PERIPH_SERIAL_H */
