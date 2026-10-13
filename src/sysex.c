#include "patch.h"

/* MIDI messages retain the stock receiver layout and stock handlers. */
void midi_handle_note_off(void *, const u8 *);
void midi_handle_note_on(void *, const u8 *);
void midi_handle_control_change(void *, const u8 *);
void midi_handle_program_change(void *, const u8 *);
void midi_handle_channel_pressure(void *, const u8 *);
void midi_handle_pitch_bend(void *, const u8 *);
void stock_cv_transmit(void *);
void stock_program_transmit(void *, u32);

static u8 rx_frame[64];
static u32 rx_size, rx_active, rx_discard, rx_ready, rx_skip;
#define TX_BYTES 2048u
static u8 tx[TX_BYTES];
static u32 tx_read, tx_write, tx_stock_lock;
static void *pending_manager;
static u32 pending_preset;

static void channel_byte(u8 *rx, u8 byte)
{
    u8 *message = rx + MIDI_RX_MESSAGE_BYTES;
    u32 *count = (u32 *)(rx + MIDI_RX_RECEIVED_COUNT);
    u32 *expected = (u32 *)(rx + MIDI_RX_EXPECTED_COUNT);
    if (byte & MIDI_STATUS_BIT) {
        u32 nibble = byte >> MIDI_STATUS_NIBBLE_SHIFT;
        message[0] = byte; *count = 1;
        *(u32 *)(rx + MIDI_RX_STATUS_NIBBLE) = nibble;
        *expected = ((u32 *)(rx + MIDI_RX_LENGTH_TABLE))[nibble - MIDI_STATUS_NIBBLE_FIRST];
        if (byte >= MIDI_SYSTEM_STATUS) return;
        if (rx[MIDI_RX_CHANNEL_MODE] == MIDI_MODE_OMNI)
            message[0] = (byte & ~MIDI_CHANNEL_MASK) | (rx[MIDI_RX_BASIC_CHANNEL] & MIDI_CHANNEL_MASK);
        else if (rx[MIDI_RX_CHANNEL_MODE] == MIDI_MODE_UNI &&
                 (byte & MIDI_CHANNEL_MASK) != rx[MIDI_RX_BASIC_CHANNEL])
            message[0] = MIDI_RX_FILTERED_STATUS;
        return;
    }
    /* Data with no channel status: before the first status, or under a
     * system or Uni-filtered status. */
    if (message[0] < MIDI_STATUS_BIT || message[0] >= MIDI_SYSTEM_STATUS) {
        counters.rx_stray_data++; return;
    }
    message[(*count)++] = byte;
    if (*count != *expected) return;
    void *setup = *(void **)(rx + MIDI_RX_ACTIVE_SETUP_POINTER);
    /* An update ignores playback. */
    if (update_active()) { *count = 1; return; }
    switch (message[0] >> MIDI_STATUS_NIBBLE_SHIFT) {
    case 8: midi_handle_note_off(setup, message); break;
    case 9: midi_handle_note_on(setup, message); break;
    case 11: midi_handle_control_change(setup, message); break;
    case 12: midi_handle_program_change(setup, message); break;
    case 13: midi_handle_channel_pressure(setup, message); break;
    case 14: midi_handle_pitch_bend(setup, message); break;
    }
    *count = 1;
}

/* Data bytes after a channel status, from the stock receiver's table. */
static u32 channel_data_bytes(u8 *rx, u8 status)
{
    return ((u32 *)(rx + MIDI_RX_LENGTH_TABLE))
        [(status >> MIDI_STATUS_NIBBLE_SHIFT) - MIDI_STATUS_NIBBLE_FIRST] - 1u;
}

void patch_rx_poll(void *receiver)
{
    /* One completed frame is held until the immediately following panel
     * hook consumes it. A bounded raw-byte budget includes rejected data. */
    for (u32 budget = 0; budget < 32 && !rx_ready; ++budget) {
        int incoming = uart0_read_byte_nonblocking();
        if (incoming < 0) break;
        u8 byte = (u8)incoming;
        counters.rx_bytes++;
        if (byte >= MIDI_REALTIME_FIRST) continue;
        if (byte == MIDI_SYSEX_START) {
            if (rx_active) counters.rx_sysex_rejects++;
            rx_active = 1; rx_size = rx_discard = rx_skip = 0;
            channel_byte(receiver, byte); /* cancel running status */
            continue;
        }
        if (byte == MIDI_SYSEX_END && rx_active) {
            rx_active = 0;
            if (!rx_discard) { rx_ready = 1; counters.rx_sysex_frames++; }
            continue;
        }
        if (rx_active && update_active() && byte < MIDI_SYSTEM_STATUS && (byte & MIDI_STATUS_BIT)) {
            /* During an update, playback merged into the update's SysEx on
             * the shared cable is skipped rather than ending the frame. */
            rx_skip = channel_data_bytes(receiver, byte);
            continue;
        }
        if (rx_skip && !(byte & MIDI_STATUS_BIT)) { rx_skip--; continue; }
        rx_skip = 0;
        if (byte & MIDI_STATUS_BIT) {
            if (rx_active) counters.rx_sysex_rejects++;
            rx_active = rx_discard = 0;
            channel_byte(receiver, byte);
        } else if (rx_active) {
            if (rx_size == sizeof(rx_frame) - 2 && !rx_discard) {
                counters.rx_sysex_overflows++; rx_discard = 1;
            }
            if (!rx_discard) rx_frame[rx_size++] = byte;
        } else channel_byte(receiver, byte);
    }
}

int sysex_send(u32 command, u32 sequence, const u8 *payload, u32 bytes)
{
    u8 frame[160]; u32 length = 0;
    frame[length++] = MIDI_SYSEX_START; frame[length++] = SYSEX_MANUFACTURER;
    frame[length++] = SYSEX_DEVICE_FIRST; frame[length++] = SYSEX_DEVICE_SECOND;
    frame[length++] = PATCH_PROTOCOL_VERSION;
    frame[length++] = (u8)command; frame[length++] = (u8)sequence;
    frame[length++] = (u8)bytes;
    for (u32 offset = 0; offset < bytes;) {
        u32 high = length++; frame[high] = 0;
        for (u32 bit = 0; bit < 7 && offset < bytes; ++bit, ++offset) {
            frame[high] |= ((payload[offset] >> 7) & 1u) << bit;
            frame[length++] = payload[offset] & 127;
        }
    }
    frame[length++] = MIDI_SYSEX_END;
    u32 irq = cpu_irq_save();
    if (length > TX_BYTES - (tx_write - tx_read)) {
        counters.tx_frame_drops++; cpu_irq_restore(irq); return 0;
    }
    for (u32 i = 0; i < length; ++i) tx[(tx_write + i) & (TX_BYTES - 1)] = frame[i];
    MEMORY_BARRIER();
    tx_write += length;
    cpu_irq_restore(irq);
    return 1;
}

void reply(u32 command, u32 sequence, u32 status, const void *data, u32 bytes)
{
    u8 buffer[REPLY_BYTES];
    buffer[0] = (u8)status;
    memcpy(buffer + 1, data, bytes);
    sysex_send(command | SYSEX_REPLY_FLAG, sequence, buffer, bytes + 1);
}

int sysex_tx_idle(void)
{
    return tx_read == tx_write && !tx_stock_lock &&
        (REG32(UART0_BASE + UART_LSR) & UART_LSR_TX_EMPTY);
}

static void dispatch_frame(void)
{
    u8 payload[REQUEST_BYTES];
    if (rx_size < SYSEX_HEADER_BYTES || rx_frame[0] != SYSEX_MANUFACTURER ||
        rx_frame[1] != SYSEX_DEVICE_FIRST || rx_frame[2] != SYSEX_DEVICE_SECOND ||
        rx_frame[3] != PATCH_PROTOCOL_VERSION || rx_frame[4] >= SYSEX_REPLY_FLAG) goto reject;
    u32 bytes = rx_frame[6], offset = SYSEX_HEADER_BYTES;
    if (bytes > REQUEST_BYTES || rx_size != SYSEX_HEADER_BYTES + bytes + (bytes + 6) / 7) goto reject;
    for (u32 i = 0; i < bytes;) {
        u32 high = rx_frame[offset++], used = 0;
        for (; used < 7 && i < bytes; ++used, ++i)
            payload[i] = rx_frame[offset++] | (((high >> used) & 1u) << 7);
        if (high >> used) goto reject;
    }
    commands_handle(rx_frame[4], rx_frame[5], payload, bytes);
    return;
reject:
    counters.rx_sysex_rejects++;
}

void sysex_panel_service(void)
{
    if (!counters.panel_polls) trace_stage(TRACE_STAGE_FIRST_PANEL_SERVICE);
    counters.panel_polls++;
    if (rx_ready) { dispatch_frame(); rx_ready = 0; }
    commands_panel_tick();
    status_panel_tick();
    record_panel_tick();
    panel_tick();
    if (!tx_stock_lock) {
        u32 room = UART_FIFO_DEPTH - REG32(UART0_BASE + UART_TX_FIFO_LVL);
        while (room-- && tx_read != tx_write) {
            u8 byte = tx[tx_read++ & (TX_BYTES - 1)];
            /* SysEx cancels receivers' running status; make stock resend it. */
            if (byte == MIDI_SYSEX_START) BYTE(MIDI_MANAGER + MIDI_MANAGER_TX_RUNNING_STATUS) = 0;
            REG32(UART0_BASE + UART_THR) = byte;
            counters.tx_sysex_bytes++;
        }
        if (tx_read == tx_write && pending_manager &&
            REG32(UART0_BASE + UART_TX_FIFO_LVL) <= UART_FIFO_DEPTH - 8) {
            void *manager = pending_manager; pending_manager = 0;
            patch_program_transmit(manager, pending_preset);
        }
    }
}

static int stock_tx_claim(void)
{
    u32 irq = cpu_irq_save();
    int ready = tx_read == tx_write && !tx_stock_lock;
    if (ready) tx_stock_lock = 1;
    cpu_irq_restore(irq);
    return ready;
}
void patch_cv_transmit(void *manager)
{
    if (!stock_tx_claim()) return;
    stock_cv_transmit(manager);
    tx_stock_lock = 0;
}
void patch_program_transmit(void *manager, u32 preset)
{
    if (!stock_tx_claim()) {
        u32 irq = cpu_irq_save();
        pending_preset = preset; pending_manager = manager;
        cpu_irq_restore(irq); return;
    }
    stock_program_transmit(manager, preset);
    tx_stock_lock = 0;
}
void patch_uart_put(u32 base, u32 byte)
{
    REG32(base) = byte;
    counters.tx_stock_bytes++;
}
