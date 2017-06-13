/*
 This project is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 Deviation is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with Deviation.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef MODULAR
  //Allows the linker to properly relocate
  #define E012_Cmds PROTO_Cmds
  #pragma long_calls
#endif
#include "common.h"
#include "interface.h"
#include "mixer.h"
#include "config/model.h"
#include "config/tx.h" // for Transmitter
#include "music.h"
#include "telemetry.h"

#ifdef MODULAR
  //Some versions of gcc applythis to definitions, others to calls
  //So just use long_calls everywhere
  //#pragma long_calls_off
  extern unsigned _data_loadaddr;
  const unsigned long protocol_type = (unsigned long)&_data_loadaddr;
#endif

#ifdef PROTO_HAS_NRF24L01

#include "iface_nrf24l01.h"

#ifdef EMULATOR
    #define USE_FIXED_MFGID
    #define BIND_COUNT 4
    #define dbgprintf printf
#else
    #define BIND_COUNT 500
    //printf inside an interrupt handler is really dangerous
    //this shouldn't be enabled even in debug builds without explicitly
    //turning it on
    #define dbgprintf if(0) printf
#endif

// stock Tx has 4525us interval between packets
// we send at higher rate to mitigate hs6200 having 
// a hard time to decode packets sent by a nrf24l01
#define PACKET_PERIOD       1000
#define INITIAL_WAIT        500
#define RF_BIND_CHANNEL     0x3c
#define ADDRESS_LENGTH      5
#define NUM_RF_CHANNELS     4
#define PACKET_SIZE         15

static const u8 bind_address[ADDRESS_LENGTH] = {0x55,0x42,0x9C,0x8F,0xC9};
static u8 tx_addr[ADDRESS_LENGTH];
static u8 rf_chans[NUM_RF_CHANNELS];
static u8 packet[PACKET_SIZE];
static u8 phase;
static u16 bind_counter;
static u8 tx_power;
static u8 current_chan;

enum {
    BIND,
    DATA
};

// For code readability
enum {
    CHANNEL1 = 0,   // Aileron
    CHANNEL2,       // Elevator
    CHANNEL3,       // Throttle
    CHANNEL4,       // Rudder
    CHANNEL5,       // 
    CHANNEL6,       // Flip
    CHANNEL7,       //
    CHANNEL8,       //
    CHANNEL9,       // Headless
    CHANNEL10,      // RTH
};

#define CHANNEL_FLIP     CHANNEL6
#define CHANNEL_HEADLESS CHANNEL9
#define CHANNEL_RTH      CHANNEL10

// Bit vector from bit position
#define BV(bit) (1 << bit)

// HS6200 emulation layer
static u8 hs6200_crc;
static u16 hs6200_crc_init;
static const u16 crc_poly = 0x1021;
static u8 hs6200_tx_addr[5];

static const u8 hs6200_scramble[] = {
    0x80,0xf5,0x3b,0x0d,0x6d,0x2a,0xf9,0xbc,
    0x51,0x8e,0x4c,0xfd,0xc1,0x65,0xd0}; // todo: find all 32 bytes

static u16 crc_update(u16 crc, u8 byte, u8 bits)
{
  crc = crc ^ (byte << 8);
  while(bits--)
    if((crc & 0x8000) == 0x8000) 
        crc = (crc << 1) ^ crc_poly;
    else 
        crc = crc << 1;
  return crc;
}

static void HS6200_SetTXAddr(const u8* addr, u8 len)
{
    NRF24L01_WriteRegisterMulti(NRF24L01_10_TX_ADDR, addr, len);
    // precompute address crc
    hs6200_crc_init = 0xffff;
    for(int i=0; i<len; i++)
        hs6200_crc_init = crc_update(hs6200_crc_init, addr[len-1-i], 8);
    memcpy(hs6200_tx_addr, addr, len);
}

static u16 hs6200_calc_crc(u8* msg, u8 len)
{
    u8 pos;
    u16 crc = hs6200_crc_init;
    
    // pcf + payload
	for(pos=0; pos < len-1; pos++) { 
		crc = crc_update(crc, msg[pos], 8);
	}
    // last byte (1 bit only)
    if(len > 0) {
        crc = crc_update(crc, msg[pos+1], 1);
    }
    
    return crc;
}

void HS6200_Configure(u8 flags)
{
    hs6200_crc = !!(flags & BV(NRF24L01_00_EN_CRC));
    flags &= ~(BV(NRF24L01_00_EN_CRC) | BV(NRF24L01_00_CRCO));
    NRF24L01_WriteReg(NRF24L01_00_CONFIG, flags & 0xff);      
}

static u8 HS6200_WritePayload(u8* msg, u8 len)
{
    u8 payload[32];
    const u8 no_ack = 1; // never ask for an ack
    static u8 pid;
    u8 pos = 0;
    
    // guard bytes
    payload[pos++] = hs6200_tx_addr[0]; // todo: manage custom guard bytes
    payload[pos++] = hs6200_tx_addr[0]; // todo: manage custom guard bytes
    
    // packet control field
    payload[pos++] = ((len & 0x3f) << 2) | (pid & 0x03);
    payload[pos] = (no_ack & 0x01) << 7;
    pid++;
    
    // scrambled payload
    if(len > 0) {
        payload[pos++] |= (msg[0] ^ hs6200_scramble[0]) >> 1; 
        for(u8 i=1; i<len; i++)
            payload[pos++] = ((msg[i-1] ^ hs6200_scramble[i-1]) << 7) | ((msg[i] ^ hs6200_scramble[i]) >> 1);
        payload[pos] = (msg[len-1] ^ hs6200_scramble[len-1]) << 7; 
    }
    
    // crc
    if(hs6200_crc) {
        u16 crc = hs6200_calc_crc(&payload[2], len+2);
        uint8_t hcrc = crc >> 8;
        uint8_t lcrc = crc & 0xff;
        payload[pos++] |= (hcrc >> 1);
        payload[pos++] = (hcrc << 7) | (lcrc >> 1);
        payload[pos++] = lcrc << 7;
    }
    
    return NRF24L01_WritePayload(payload, pos);
}
// end of HS6200 emulation layer

#define CHAN_RANGE (CHAN_MAX_VALUE - CHAN_MIN_VALUE)
static u16 scale_channel(u8 ch, u16 destMin, u16 destMax)
{
    s32 chanval = Channels[ch];
    s32 range = (s32) destMax - (s32) destMin;

    if (chanval < CHAN_MIN_VALUE)
        chanval = CHAN_MIN_VALUE;
    else if (chanval > CHAN_MAX_VALUE)
        chanval = CHAN_MAX_VALUE;
    return (range * (chanval - CHAN_MIN_VALUE)) / CHAN_RANGE + destMin;
}

#define GET_FLAG(ch, mask) (Channels[ch] > 0 ? mask : 0)
static void send_packet(u8 bind)
{
    packet[0] = tx_addr[1];
    if(bind) {
        packet[1] = 0xaa;
        memcpy(&packet[2], rf_chans, NUM_RF_CHANNELS);
        memcpy(&packet[6], tx_addr, ADDRESS_LENGTH);
    }
    else {
        packet[1] = 0x01
                  | GET_FLAG(CHANNEL_RTH, 0x04)
                  | GET_FLAG(CHANNEL_HEADLESS, 0x10)
                  | GET_FLAG(CHANNEL_FLIP, 0x40);
        packet[2] = scale_channel(CHANNEL1, 0xc8, 0x00); // aileron
        packet[3] = scale_channel(CHANNEL2, 0x00, 0xc8); // elevator
        packet[4] = scale_channel(CHANNEL4, 0xc8, 0x00); // rudder
        packet[5] = scale_channel(CHANNEL3, 0x00, 0xc8); // throttle
        packet[6] = 0xaa;
        packet[7] = 0x02; // rate (0-2)
        packet[8] = 0x00;
        packet[9] = 0x00;
        packet[10]= 0x00;
    }
    packet[11] = 0x00;
    packet[12] = 0x00;
    packet[13] = 0x56; 
    packet[14] = tx_addr[2];
    
    // Power on, TX mode, CRC enabled
    HS6200_Configure(BV(NRF24L01_00_EN_CRC) | BV(NRF24L01_00_CRCO) | BV(NRF24L01_00_PWR_UP));
    NRF24L01_WriteReg(NRF24L01_05_RF_CH, bind ? RF_BIND_CHANNEL : rf_chans[current_chan++]);
    current_chan %= NUM_RF_CHANNELS;
    
    NRF24L01_WriteReg(NRF24L01_07_STATUS, 0x70);
    NRF24L01_FlushTx();
    
    HS6200_WritePayload(packet, PACKET_SIZE);
    
    // Check and adjust transmission power. We do this after
    // transmission to not bother with timeout after power
    // settings change -  we have plenty of time until next
    // packet.
    if(tx_power > TXPOWER_10mW)
        tx_power = TXPOWER_10mW;
    if (tx_power != Model.tx_power) {
        //Keep transmit power updated
        tx_power = Model.tx_power;
        NRF24L01_SetPower(tx_power);
    }
}

static void e012_init()
{
    NRF24L01_Initialize();
    NRF24L01_SetTxRxMode(TX_EN);
    HS6200_SetTXAddr(bind_address, ADDRESS_LENGTH);
    NRF24L01_FlushTx();
    NRF24L01_FlushRx();
    NRF24L01_WriteReg(NRF24L01_07_STATUS, 0x70);     // Clear data ready, data sent, and retransmit
    NRF24L01_WriteReg(NRF24L01_01_EN_AA, 0x00);      // No Auto Acknowldgement on all data pipes
    NRF24L01_WriteReg(NRF24L01_03_SETUP_AW, 0x03);
    NRF24L01_WriteReg(NRF24L01_04_SETUP_RETR, 0x00); // no retransmits
    NRF24L01_SetBitrate(NRF24L01_BR_1M);             // 1 Mbps
    NRF24L01_SetPower(Model.tx_power);
    NRF24L01_Activate(0x73);                          // Activate feature register
    NRF24L01_WriteReg(NRF24L01_1C_DYNPD, 0x00);       // Disable dynamic payload length on all pipes
    NRF24L01_WriteReg(NRF24L01_1D_FEATURE, 0x01);     // Set feature bits on
    NRF24L01_Activate(0x73);
    
    // Check for Beken BK2421/BK2423 chip
    // It is done by using Beken specific activate code, 0x53
    // and checking that status register changed appropriately
    // There is no harm to run it on nRF24L01 because following
    // closing activate command changes state back even if it
    // does something on nRF24L01
    NRF24L01_Activate(0x53); // magic for BK2421 bank switch
    dbgprintf("Trying to switch banks\n");
    if (NRF24L01_ReadReg(NRF24L01_07_STATUS) & 0x80) {
        dbgprintf("BK2421 detected\n");
        // Beken registers don't have such nice names, so we just mention
        // them by their numbers
        // It's all magic, eavesdropped from real transfer and not even from the
        // data sheet - it has slightly different values
        NRF24L01_WriteRegisterMulti(0x00, (u8 *) "\x40\x4B\x01\xE2", 4);
        NRF24L01_WriteRegisterMulti(0x01, (u8 *) "\xC0\x4B\x00\x00", 4);
        NRF24L01_WriteRegisterMulti(0x02, (u8 *) "\xD0\xFC\x8C\x02", 4);
        NRF24L01_WriteRegisterMulti(0x03, (u8 *) "\x99\x00\x39\x21", 4);
        NRF24L01_WriteRegisterMulti(0x04, (u8 *) "\xD9\x96\x82\x1B", 4);
        NRF24L01_WriteRegisterMulti(0x05, (u8 *) "\x24\x06\x7F\xA6", 4);
        NRF24L01_WriteRegisterMulti(0x0C, (u8 *) "\x00\x12\x73\x00", 4);
        NRF24L01_WriteRegisterMulti(0x0D, (u8 *) "\x46\xB4\x80\x00", 4);
        NRF24L01_WriteRegisterMulti(0x04, (u8 *) "\xDF\x96\x82\x1B", 4);
        NRF24L01_WriteRegisterMulti(0x04, (u8 *) "\xD9\x96\x82\x1B", 4);
    } else {
        dbgprintf("nRF24L01 detected\n");
    }
    NRF24L01_Activate(0x53); // switch bank back
}

static void initialize_txid()
{
    u32 lfsr = 0xb2c54a2ful;
    u8 i,j;
    
#ifndef USE_FIXED_MFGID
    u8 var[12];
    MCU_SerialNumber(var, 12);
    dbgprintf("Manufacturer id: ");
    for (i = 0; i < 12; ++i) {
        dbgprintf("%02X", var[i]);
        rand32_r(&lfsr, var[i]);
    }
    dbgprintf("\r\n");
#endif

    if (Model.fixed_id) {
       for (i = 0, j = 0; i < sizeof(Model.fixed_id); ++i, j += 8)
           rand32_r(&lfsr, (Model.fixed_id >> j) & 0xff);
    }
    // Pump zero bytes for LFSR to diverge more
    for (i = 0; i < sizeof(lfsr); ++i) rand32_r(&lfsr, 0);

    // tx address
    for(i=0; i<4; i++)
        tx_addr[i] = (lfsr >> (i*8)) & 0xff;
    rand32_r(&lfsr, 0);
    tx_addr[4] = lfsr & 0xff;
    
    // rf channels 
    // hack: use only 1 out of 4 channels as it seems the hs6200
    // has a hard time decoding packets sent by the nrf24l01
    rand32_r(&lfsr, 0);
    for(i=0; i<NUM_RF_CHANNELS; i++) {
        //rf_chans[i] = 0x30 + (((lfsr >> (i*8)) & 0xff) % 0x21); 
        rf_chans[i] = lfsr % 0x51; // hack
    }
}

MODULE_CALLTYPE
static u16 e012_callback()
{
    switch (phase) {
        case BIND:
            if (bind_counter == 0) {
                HS6200_SetTXAddr(tx_addr, 5);
                phase = DATA;
                PROTOCOL_SetBindState(0);
            } else {
                send_packet(1);
                bind_counter--;
            }
            break;
        case DATA:
            send_packet(0);
            break;
    }
    return PACKET_PERIOD;
}

static void initialize()
{
    CLOCK_StopTimer();
    tx_power = Model.tx_power;
    // hs6200 has a hard time decoding packets sent by nrf24l01
    // if Tx power is set above 10mW, limit it 
    if(tx_power > TXPOWER_10mW)
        tx_power = TXPOWER_10mW;
    initialize_txid();
    e012_init();
    bind_counter = BIND_COUNT;
    current_chan = 0;
    PROTOCOL_SetBindState(BIND_COUNT * PACKET_PERIOD / 1000);
    phase = BIND;
    CLOCK_StartTimer(INITIAL_WAIT, e012_callback);
}

const void *E012_Cmds(enum ProtoCmds cmd)
{
    switch(cmd) {
        case PROTOCMD_INIT:  initialize(); return 0;
        case PROTOCMD_DEINIT:
        case PROTOCMD_RESET:
            CLOCK_StopTimer();
            return (void *)(NRF24L01_Reset() ? 1L : -1L);
        case PROTOCMD_CHECK_AUTOBIND: return (void *)1L; // always Autobind
        case PROTOCMD_BIND:  initialize(); return 0;
        case PROTOCMD_NUMCHAN: return (void *) 10L; // A, E, T, R, n/a , flip, n/a, n/a, headless, RTH
        case PROTOCMD_DEFAULT_NUMCHAN: return (void *)10L;
        case PROTOCMD_CURRENT_ID: return Model.fixed_id ? (void *)((unsigned long)Model.fixed_id) : 0;
        case PROTOCMD_GETOPTIONS: return (void *)0L;
        case PROTOCMD_TELEMETRYSTATE: return (void *)(long)PROTO_TELEM_UNSUPPORTED;
        default: break;
    }
    return 0;
}

#endif
