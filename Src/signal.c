/*
 * IO.c
 *
 *  Created on: Sep. 26, 2020
 *      Author: Alka
 */

#include "signal.h"
#include "IO.h"
#include "common.h"
#include "dshot.h"
#include "functions.h"
#include "serial_telemetry.h"
#include "sounds.h"
#include "targets.h"
int max_servo_deviation = 250;
int servorawinput;
uint16_t smallestnumber = 20000;
uint8_t enter_calibration_count = 0;
uint8_t calibration_required = 0;
uint8_t high_calibration_counts = 0;
uint8_t high_calibration_set = 0;
uint16_t last_high_threshold = 0;
uint8_t low_calibration_counts = 0;
uint16_t last_input = 0;
char output_timer_prescaler;
uint8_t buffersize = 32;
uint32_t average_signal_pulse;
uint8_t average_count;
uint32_t average_packet_length;
uint16_t dshot_frametime_high = 50000;
uint16_t dshot_frametime_low = 0;

void computeMSInput()
{

    int lastnumber = dma_buffer[0];
    for (int j = 1; j < 2; j++) {

        if (((dma_buffer[j] - lastnumber) < 1500) && ((dma_buffer[j] - lastnumber) > 0)) { // blank space

            newinput = map((dma_buffer[j] - lastnumber), 243, 1200, 0, 2000);
            break;
        }
        lastnumber = dma_buffer[j];
    }
}

#ifdef CL_SERVO_TONES
// CL-ESC: pipani motorem na povel z casovace.
//
// Casovac jede v pasmu 1200-2000 us, takze vsechno pod 1180 us je volne a da se
// pouzit jako povelovy kanal. Tri pasma = tri ruzne tony; mezi nimi jsou mezery
// 20 us, aby zaokrouhleni generatoru serva na ESP32 (jednotky us, meni se s
// opakovaci frekvenci) nikdy nepreteklo do sousedniho pasma.
//
//   1000-1040 us -> ton 3 (melodie, chyba casovace)
//   1060-1100 us -> ton 2 (odpocet, posledni sekundy)
//   1120-1160 us -> ton 1 (odpocet spusten)
//
// Povel plati az po CL_TONE_DEBOUNCE snimcich v pasmu a spusti se jen jednou pri
// vstupu do nej, takze drzeni pasma nepipa dokola. Vsechna pasma lezi pod prahem
// stopu, takze je ESC porad cte jako nulovy plyn a armovani to nijak nerusi.
//
// BEZPECNOST: pipani roztaci civky motoru, proto se povel ignoruje, kdyz motor
// bezi. Za letu tedy nejde vyvolat ani chybou v casovaci.
static uint8_t cl_tone_count = 0;
static uint8_t cl_tone_band = 0;

static void clServoTone(uint16_t pulse)
{
    uint8_t band = 0;
    if (pulse >= 1000 && pulse <= 1040) {
        band = 3;
    } else if (pulse >= 1060 && pulse <= 1100) {
        band = 2;
    } else if (pulse >= 1120 && pulse <= 1160) {
        band = 1;
    }

    if (band == 0) { // mimo pasmo - pripravit se na dalsi povel
        cl_tone_band = 0;
        cl_tone_count = 0;
        return;
    }
    if (band != cl_tone_band) { // zmena pasma zacina pocitat znovu
        cl_tone_band = band;
        cl_tone_count = 0;
    }
    if (cl_tone_count < CL_TONE_DEBOUNCE) {
        if (++cl_tone_count == CL_TONE_DEBOUNCE && !running) {
            play_tone_flag = band;
        }
    }
}
#endif

#ifdef CL_SERVO_CONFIG
// ---------------------------------------------------------------------------
// CL-ESC: datovy kanal na signalovem vodici - nastaveni regulatoru z casovace.
//
// Duvod: nastaveni jako motor_kv nebo pocet polu se dnes da zmenit jen programatorem
// nebo konfiguratorem pres bootloader, tedy s modelem na stole a rozebranym drakem.
// Casovac ale uz je k regulatoru pripojeny dvema vodici - signalem a telemetrii -
// a oba jsou v modelu natrvalo. Tenhle kanal je vyuzije, takze se nic neprepajuje:
//
//   casovac -> ESC   povel po signalovem vodici (tady)
//   ESC -> casovac   48 bajtu EEPROM v info paketu po telemetrii (makeInfoPacket)
//
// Kodovani: kazdy servo snimek nese jeden symbol podle delky pulzu.
//
//   805-955 us, krok 10 us  -> datovy pulnibble 0-15 (16 symbolu)
//   975 us                  -> SYNC, zacatek ramce
//   cokoli jineho           -> mezera
//
// Vsechny symboly lezi pod prahem stopu i pod pasmy tonu (od 1000 us), takze je
// regulator porad cte jako nulovy plyn a pipani to nijak nekrizi. Casovac mezi
// symboly vklada svuj klidovy pulz (1200 us) jako mezeru, aby sly dva stejne
// symboly za sebou rozlisit.
//
// Ramec = SYNC + 8 nibblu = cmd, addr, val, crc8. Pri 250 Hz a peti snimcich na
// symbol trva jeden povel asi 180 ms.
//
// BEZPECNOST:
//  - povel se prijme az po dvou shodnych snimcich, takze jeden ruseny pulz nic nezmeni,
//  - ramec chrani CRC8 (stejny polynom jako telemetrie),
//  - za behu motoru se nic neaplikuje,
//  - zapisovat lze jen bajty 5-47. Bajt 0 je branou bootloaderu a bajty 1-4 nesou
//    verzi; prepsat je znamena rozbit desku tak, ze uz ji casovac nespravi.
// ---------------------------------------------------------------------------
#define CL_SYM_BASE 805
#define CL_SYM_STEP 10
#define CL_SYM_TOL 4
#define CL_SYM_SYNC 975

extern uint8_t get_crc8(uint8_t* Buf, uint8_t BufLen);

volatile uint8_t cl_cfg_cmd = 0;
volatile uint8_t cl_cfg_addr = 0;
volatile uint8_t cl_cfg_val = 0;

// Vraci 0-15 pro datovy symbol, 16 pro SYNC, -1 pro mezeru.
static int8_t clSymbol(uint16_t pulse)
{
    if (pulse >= CL_SYM_SYNC - CL_SYM_TOL && pulse <= CL_SYM_SYNC + CL_SYM_TOL) {
        return 16;
    }
    if (pulse < CL_SYM_BASE - CL_SYM_TOL) {
        return -1;
    }
    int16_t d = (int16_t)pulse - CL_SYM_BASE;
    int16_t n = (d + CL_SYM_STEP / 2) / CL_SYM_STEP;
    if (n < 0 || n > 15) {
        return -1;
    }
    int16_t err = d - n * CL_SYM_STEP;
    if (err > CL_SYM_TOL || err < -CL_SYM_TOL) { // mezi mrizkou - radeji zahodit
        return -1;
    }
    return (int8_t)n;
}

static int8_t cl_sym_held = -1;  // symbol drzeny v predchozich snimcich
static uint8_t cl_sym_count = 0; // kolik snimcu za sebou uz drzi
static uint8_t cl_sym_taken = 0; // uz byl prijat, ceka se na mezeru
static uint8_t cl_nib[8];
static uint8_t cl_nib_count = 0;
static uint8_t cl_rx_active = 0;

static void clServoConfig(uint16_t pulse)
{
    int8_t sym = clSymbol(pulse);

    if (sym < 0) { // mezera - dalsi symbol smi zacit
        cl_sym_held = -1;
        cl_sym_count = 0;
        cl_sym_taken = 0;
        return;
    }
    if (sym != cl_sym_held) {
        cl_sym_held = sym;
        cl_sym_count = 0;
        cl_sym_taken = 0;
    }
    if (cl_sym_taken || ++cl_sym_count < 3) { // tri shodne snimky: jeden zakmit nesmi projit jako symbol
        return;
    }
    cl_sym_taken = 1;

    if (sym == 16) { // SYNC zacina ramec kdykoli, i uprostred rozdelaneho
        cl_rx_active = 1;
        cl_nib_count = 0;
        return;
    }
    if (!cl_rx_active) {
        return;
    }
    cl_nib[cl_nib_count++] = (uint8_t)sym;
    if (cl_nib_count < 8) {
        return;
    }
    cl_rx_active = 0;

    uint8_t body[3];
    body[0] = (cl_nib[0] << 4) | cl_nib[1]; // cmd
    body[1] = (cl_nib[2] << 4) | cl_nib[3]; // addr
    body[2] = (cl_nib[4] << 4) | cl_nib[5]; // val
    if (get_crc8(body, 3) != (uint8_t)((cl_nib[6] << 4) | cl_nib[7])) {
        return;
    }
    // Sum v datovem pasmu obcas poskladá ramec, ktery CRC8 projde - je to jeden
    // pripad z 256. Kontrola rozsahu povelu z toho ubere dalsich sestnact
    // sedmnactin a stoji jedno porovnani.
    if (body[0] < CL_CFG_SET || body[0] > CL_CFG_READ) {
        return;
    }
    if (running) { // za letu se nenastavuje nic
        return;
    }
    // Zapis flash i pretazeni nastaveni patri do hlavni smycky, ne do preruseni
    // od vstupniho zachytu - mazani stranky trva desitky ms.
    cl_cfg_addr = body[1];
    cl_cfg_val = body[2];
    cl_cfg_cmd = body[0]; // az jako posledni, hlavni smycka ceka na nej
}
#endif

void computeServoInput()
{
    if (((dma_buffer[1] - dma_buffer[0]) > 800) && ((dma_buffer[1] - dma_buffer[0]) < 2200)) {
				signaltimeout = 0;
#ifdef CL_SERVO_TONES
        clServoTone(dma_buffer[1] - dma_buffer[0]);
#endif
#ifdef CL_SERVO_CONFIG
        clServoConfig(dma_buffer[1] - dma_buffer[0]);
#endif
        if (calibration_required) {
            if (!high_calibration_set) {
                if (high_calibration_counts == 0) {
                    last_high_threshold = dma_buffer[1] - dma_buffer[0];
                }
                high_calibration_counts++;
                if (getAbsDif(last_high_threshold, servo_high_threshold) > 50) {
                    calibration_required = 0;
                } else {
                    servo_high_threshold = ((7 * servo_high_threshold + (dma_buffer[1] - dma_buffer[0])) >> 3);
                    if (high_calibration_counts > 50) {
                        servo_high_threshold = servo_high_threshold - 25;
                        eepromBuffer.servo.high_threshold = (servo_high_threshold - 1750) / 2;
                        high_calibration_set = 1;
                        playDefaultTone();
                    }
                }
                last_high_threshold = servo_high_threshold;
            }
            if (high_calibration_set) {
                if (dma_buffer[1] - dma_buffer[0] < 1250) {
                    low_calibration_counts++;
                    servo_low_threshold = ((7 * servo_low_threshold + (dma_buffer[1] - dma_buffer[0])) >> 3);
                }
                if (low_calibration_counts > 75) {
                    servo_low_threshold = servo_low_threshold + 25;
                    eepromBuffer.servo.low_threshold = (servo_low_threshold - 750) / 2;
                    calibration_required = 0;
                    saveEEpromSettings();
                    low_calibration_counts = 0;
                    playChangedTone();
                }
            }
            signaltimeout = 0;
        } else {
            if (eepromBuffer.bi_direction) {
                if (dma_buffer[1] - dma_buffer[0] <= servo_neutral) {
                    servorawinput = map((dma_buffer[1] - dma_buffer[0]),
                        servo_low_threshold, servo_neutral, 0, 1000);
                } else {
                    servorawinput = map((dma_buffer[1] - dma_buffer[0]), servo_neutral + 1,
                        servo_high_threshold, 1001, 2000);
                }
            } else {
                servorawinput = map((dma_buffer[1] - dma_buffer[0]), servo_low_threshold,
                    servo_high_threshold, 47, 2047);
                if (servorawinput <= 48) {
                    servorawinput = 0;
                }
            }
            signaltimeout = 0;
        }
    } else {
        zero_input_count = 0; // reset if out of range
    }

    if (servorawinput - newinput > max_servo_deviation) {
        newinput += max_servo_deviation;
    } else if (newinput - servorawinput > max_servo_deviation) {
        newinput -= max_servo_deviation;
    } else {
        newinput = servorawinput;
    }
}

void transfercomplete()
{
#ifndef MCU_F031   // f031 does not use software EXTI event to process dshot
    if (armed && dshot_telemetry) {
        if (out_put) {
            receiveDshotDma();
            compute_dshot_flag = 2;
            return;
        } else {
            sendDshotDma();
            compute_dshot_flag = 1;
            return;
        }
    }
#endif
    if (inputSet == 0) {
        detectInput();
        receiveDshotDma();
        return;
    }
    if (inputSet == 1) {

        if (dshot_telemetry) {
            if (out_put) {
                make_dshot_package(e_com_time);
                computeDshotDMA();
                receiveDshotDma();
                return;
            } else {
                sendDshotDma();
                return;
            }
        } else {

            if (dshot == 1) {
                computeDshotDMA();
                receiveDshotDma();
            }
            if (servoPwm == 1) {
                if (getInputPinState()) {
                    buffersize = 3;
                } else {
                    buffersize = 2;
                    computeServoInput();
                }
                receiveDshotDma();
            }
        }
        if (!armed) {
            if (dshot && (average_count < 8) && (zero_input_count > 5)) {
                average_count++;
                average_packet_length = average_packet_length + (uint16_t)(dma_buffer[31] - dma_buffer[0]);
                if (average_count == 8) {
                    dshot_frametime_high = (average_packet_length >> 3) + (average_packet_length >> 7);
                    dshot_frametime_low = (average_packet_length >> 3) - (average_packet_length >> 7);
                }
            }
            if (adjusted_input == 0 && calibration_required == 0) { // note this in input..not newinput so it
                                                                    // will be adjusted be main loop
                zero_input_count++;
            } else {
              if(!eepromBuffer.disable_stick_calibration){
                zero_input_count = 0;
                if (adjusted_input > 1500) {
                    if (getAbsDif(adjusted_input, last_input) > 50) {
                        enter_calibration_count = 0;
                    } else {
                        enter_calibration_count++;
                    }

                    if (enter_calibration_count > 50 && (!high_calibration_set)) {
                        playBeaconTune3();
                        calibration_required = 1;
                        enter_calibration_count = 0;
                    }
                    last_input = adjusted_input;
                }
              }
            }
        }
    }
}

void checkDshot()
{
    if ((smallestnumber >= 1) && (smallestnumber < 4) && (average_signal_pulse < 60)) {
        ic_timer_prescaler = 0;
        if (CPU_FREQUENCY_MHZ > 100) {
            output_timer_prescaler = 1;
        } else {
            output_timer_prescaler = 0;
        }
        //	dshot_runout_timer = 1000;
        dshot = 1;
        buffer_padding = 14;
        buffersize = 32;
        inputSet = 1;
    }
    if ((smallestnumber >= 4) && (smallestnumber <= 8) && (average_signal_pulse < 100)) {
        dshot = 1;
        ic_timer_prescaler = 1;
        if (CPU_FREQUENCY_MHZ > 100) {
            output_timer_prescaler = 3;
        } else {
            output_timer_prescaler = 1;
        }
        buffer_padding = 7;
        buffersize = 32;
        inputSet = 1;
    }
}
void checkServo()
{
    if (smallestnumber > 200 && smallestnumber < 20000) {
        servoPwm = 1;
        ic_timer_prescaler = CPU_FREQUENCY_MHZ - 1;
        buffersize = 2;
        inputSet = 1;
    }
}

void detectInput()
{
    smallestnumber = 20000;
    average_signal_pulse = 0;
    int lastnumber = dma_buffer[0];
    for (int j = 1; j < 31; j++) {
        if (dma_buffer[j] - lastnumber > 0) {
            if ((dma_buffer[j] - lastnumber) < smallestnumber) {
                smallestnumber = dma_buffer[j] - lastnumber;
            }
            average_signal_pulse += (dma_buffer[j] - lastnumber);
        }
        lastnumber = dma_buffer[j];
    }
    average_signal_pulse = average_signal_pulse / 32;

    if (dshot == 1) {
        checkDshot();
    }
    if (servoPwm == 1) {
        checkServo();
    }

    if (!dshot && !servoPwm) {
        checkDshot();
        checkServo();
    }
}
