#include "kiss_telemetry.h"
#include "eeprom.h"

extern uint8_t get_crc8(uint8_t* Buf, uint8_t BufLen);

uint8_t aTxBuffer[49] __attribute__((aligned(4)));
uint8_t nbDataToTransmit = sizeof(aTxBuffer);

void makeTelemPackage(int8_t temp, uint16_t voltage, uint16_t current, uint16_t consumption, uint16_t e_rpm)
{
    kiss_telem_pkt_t* telem_pkt = (kiss_telem_pkt_t*)aTxBuffer;

    telem_pkt->temperature = temp; // temperature in Celcius

    // voltage in centivolts
    telem_pkt->voltage_h = (voltage >> 8) & 0xFF;
    telem_pkt->voltage_l = voltage & 0xFF;

    // current in centiamps
    telem_pkt->current_h = (current >> 8) & 0xFF;
    telem_pkt->current_l = current & 0xFF;

    // accumulated current consumption in mAH
    telem_pkt->consumption_h = (consumption >> 8) & 0xFF;
    telem_pkt->consumption_l = consumption & 0xFF;

    // eRPM * 100, so 1 in the packet means 100 eRPM
    telem_pkt->erpm_h = (e_rpm >> 8) & 0xFF;
    telem_pkt->erpm_l = e_rpm & 0xFF;

    telem_pkt->crc = get_crc8((uint8_t*)telem_pkt, sizeof(kiss_telem_pkt_t) - 1);
}

// CL-ESC: odpoved s nastavenim po osmi bajtech.
//
// Nepouziva se k tomu 49bajtovy info paket AM32 - ten se na teto ceste nikdy
// neodesle. Overeno mereni: `send_telem_DMA(49)` neposle nic, zatimco tataz
// funkce s deseti bajty vozi telemetrii spolehlive. Info paket se v AM32
// bezne pouziva jen pres DShot, takze po seriove telemetrii zjevne nikdy nejel.
//
// Ramec ma tvar telemetrickeho, aby sel po overene ceste: znacka, osm bajtu
// EEPROM, CRC8. Znacka 0xC0-0xC5 je v poli teploty, kde by znamenala -64 az
// -59 stupnu, takze se s merenim nesplete.
void makeConfigPacket(uint8_t index)
{
    aTxBuffer[0] = 0xC0 | (index & 0x07);
    for (uint8_t i = 0; i < 8; i++) {
        aTxBuffer[1 + i] = eepromBuffer.buffer[index * 8 + i];
    }
    aTxBuffer[9] = get_crc8(aTxBuffer, 9);
}

void makeInfoPacket()
{
    for(int i = 0; i < 48; i++) {
        aTxBuffer[i] = eepromBuffer.buffer[i];
    }
    aTxBuffer[48] = get_crc8(aTxBuffer, 48);
}
