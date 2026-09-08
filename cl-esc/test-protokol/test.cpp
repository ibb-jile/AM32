// Kruzek: odesilatel z casovace -> vzorkovani servo snimku -> dekoder z AM32.
// Obe strany jsou vytazene ze skutecnych zdroju (dec.inc / snd.inc), aby test
// nemohl projit proti prepisu, ktery se od firmwaru lisi.
#include <stdio.h>
#include <string.h>
#include <stdint.h>

// --- spolecne CRC8 (AM32 Src/functions.c, polynom 0x07) ---
uint8_t get_crc8(uint8_t* Buf, uint8_t BufLen) {
  uint8_t crc = 0;
  for (uint8_t i = 0; i < BufLen; i++) {
    uint8_t c = Buf[i] ^ crc;
    for (uint8_t b = 0; b < 8; b++) c = (c & 0x80) ? (0x07 ^ (c << 1)) : (c << 1);
    crc = c;
  }
  return crc;
}
uint8_t telemCrc8(const uint8_t* b, uint8_t n) { return get_crc8((uint8_t*)b, n); }

// ---------------- strana regulatoru ----------------
#define CL_CFG_SET 0x01
#define CL_CFG_COMMIT 0x02
#define CL_CFG_DISCARD 0x03
#define CL_CFG_READ 0x04
uint8_t running = 0;
#include "dec.inc"

// ---------------- strana casovace ----------------
struct { int escTones; int minPulse; } config = { 1, 1200 };
int timerRunning = 0, motorFinished = 1;
enum { CAL_NONE } calPhase = CAL_NONE;
enum { TEST_IDLE } testState = TEST_IDLE;
int currentPulse = 0;
// ESP32Servo::writeTicks() orezava na meze z attach(). Test je musi mit taky,
// jinak by proslo i nastaveni, ktere na desce nikdy neodejde na drat.
int SERVO_MIN = 800, SERVO_MAX = 2000;
void servoWrite(int us) {
  if (us < SERVO_MIN) us = SERVO_MIN;
  if (us > SERVO_MAX) us = SERVO_MAX;
  currentPulse = us;
}
#include "snd.inc"

// ---------------- zkouska ----------------
static int prijato_n; static uint8_t prijato[64][3];

static void krok_esc(int pulse) {
  // computeServoInput() zahazuje pulzy mimo 800-2200 us
  if (pulse <= 800 || pulse >= 2200) return;
  clServoConfig((uint16_t)pulse);
  if (cl_cfg_cmd) {
    prijato[prijato_n][0] = cl_cfg_cmd;
    prijato[prijato_n][1] = cl_cfg_addr;
    prijato[prijato_n][2] = cl_cfg_val;
    prijato_n++;
    cl_cfg_cmd = 0;
  }
}

// Vraci pocet snimku, ktere prenos zabral. `snimekUs` je perioda serva,
// `loopMs` perioda hlavni smycky casovace, `zahod` kazdy N-ty snimek zahodi
// (simulace ruseni na lince).
static int prenos(unsigned long snimekUs, unsigned long loopMs, int zahod) {
  unsigned long now = 0, dalsiSnimek = 0;
  int snimku = 0, ticho = 0;
  currentPulse = config.minPulse;
  while (ticho < 200) {
    updateEscCfg(now);
    while (dalsiSnimek <= now * 1000) {   // servo vzorkuje aktualni pulz
      snimku++;
      if (!(zahod && snimku % zahod == 0)) krok_esc(currentPulse);
      dalsiSnimek += snimekUs;
    }
    if (!escCfgAktivni()) ticho++; else ticho = 0;
    now += loopMs;
  }
  return snimku;
}

static int ocekavej(const char* jmeno, uint8_t* oc, int n) {
  int ok = (prijato_n == n);
  for (int i = 0; ok && i < n; i++)
    ok = prijato[i][0] == oc[i*3] && prijato[i][1] == oc[i*3+1] && prijato[i][2] == oc[i*3+2];
  printf("%-46s %s  (prijato %d/%d)\n", jmeno, ok ? "OK  " : "CHYBA", prijato_n, n);
  if (!ok) for (int i = 0; i < prijato_n; i++)
    printf("      -> cmd %02X addr %3d val %3d\n", prijato[i][0], prijato[i][1], prijato[i][2]);
  return ok;
}

int main(void) {
  int chyb = 0;
  unsigned long t0;

  // 1) jeden povel pri 250 Hz
  prijato_n = 0; escCfgQueue(CL_CFG_SET, 26, 18);
  int sn = prenos(4000, 2, 0);
  uint8_t o1[] = { CL_CFG_SET, 26, 18 };
  chyb += !ocekavej("SET 26=18 @ 250 Hz, smycka 2 ms", o1, 1);
  printf("      delka prenosu: %d snimku = %d ms\n", sn - 200*2/4, (sn - 100)*4);

  // 2) cela davka jako ze stranky /esc
  prijato_n = 0;
  escCfgQueue(CL_CFG_SET, 26, 18);
  escCfgQueue(CL_CFG_SET, 27, 14);
  escCfgQueue(CL_CFG_COMMIT, 0x5A, 0xA5);
  escCfgQueue(CL_CFG_READ, 0, 0);
  prenos(4000, 2, 0);
  uint8_t o2[] = { CL_CFG_SET,26,18, CL_CFG_SET,27,14, CL_CFG_COMMIT,0x5A,0xA5, CL_CFG_READ,0,0 };
  chyb += !ocekavej("davka SET,SET,COMMIT,READ", o2, 4);

  // 3) hranicni hodnoty vsech nibblu (0x00 i 0xFF)
  prijato_n = 0; escCfgQueue(CL_CFG_SET, 47, 255);
  prenos(4000, 2, 0);
  uint8_t o3[] = { CL_CFG_SET, 47, 255 };
  chyb += !ocekavej("SET 47=255 (same jednicky v nibblech)", o3, 1);

  // 4) dva stejne nibbly za sebou (0x00 -> nibbly 0,0)
  prijato_n = 0; escCfgQueue(CL_CFG_SET, 0, 0);
  prenos(4000, 2, 0);
  uint8_t o4[] = { CL_CFG_SET, 0, 0 };
  chyb += !ocekavej("SET 0=0 (ctyri nuly za sebou)", o4, 1);

  // 5) pomala smycka casovace (10 ms)
  prijato_n = 0; escCfgQueue(CL_CFG_SET, 26, 18);
  prenos(4000, 10, 0);
  chyb += !ocekavej("SET 26=18, pomala smycka 10 ms", o1, 1);

  // 6) kazdy 7. snimek ztracen
  prijato_n = 0; escCfgQueue(CL_CFG_SET, 26, 18);
  prenos(4000, 2, 7);
  chyb += !ocekavej("SET 26=18, kazdy 7. snimek ztracen", o1, 1);

  // 7) 50 Hz - protokol na nej neni stavany, musi se to poznat
  prijato_n = 0; escCfgQueue(CL_CFG_SET, 26, 18);
  prenos(20000, 2, 0);
  printf("%-46s prijato %d (ocekava se 0 nebo 1, ne poskozeny povel)\n",
         "SET 26=18 @ 50 Hz", prijato_n);
  for (int i = 0; i < prijato_n; i++) {
    if (prijato[i][0] != CL_CFG_SET || prijato[i][1] != 26 || prijato[i][2] != 18) {
      printf("      CHYBA: poskozeny povel cmd %02X addr %d val %d\n",
             prijato[i][0], prijato[i][1], prijato[i][2]); chyb++;
    }
  }

  // 8) bezne letove pulzy nesmi nic dekodovat
  prijato_n = 0;
  for (int p = 801; p <= 2199; p++) krok_esc(p), krok_esc(p), krok_esc(p);
  printf("%-46s %s (prijato %d)\n", "prujezd 801-2199 us po jednom",
         prijato_n == 0 ? "OK  " : "CHYBA", prijato_n);
  chyb += (prijato_n != 0);

  // 9) tonova pasma a klidovy pulz nesmi byt symbol
  prijato_n = 0;
  int tony[] = { 1020, 1080, 1140, 1200, 1000, 1160, 1220, 1500, 2000 };
  for (int i = 0; i < 9; i++) for (int k = 0; k < 40; k++) krok_esc(tony[i]);
  printf("%-46s %s (prijato %d)\n", "tony 1020/1080/1140 + klid 1200 us",
         prijato_n == 0 ? "OK  " : "CHYBA", prijato_n);
  chyb += (prijato_n != 0);

  // 10) za behu motoru se povel nesmi aplikovat
  prijato_n = 0; running = 1; escCfgQueue(CL_CFG_SET, 26, 18);
  prenos(4000, 2, 0); running = 0;
  printf("%-46s %s (prijato %d)\n", "SET pri bezicim motoru",
         prijato_n == 0 ? "OK  " : "CHYBA", prijato_n);
  chyb += (prijato_n != 0);

  // 11) tvrdsi ztraty snimku - kde protokol prestane fungovat
  for (int z = 2; z <= 5; z++) {
    prijato_n = 0; escCfgQueue(CL_CFG_SET, 26, 18);
    prenos(4000, 2, z);
    printf("%-46s %s\n",
      z == 2 ? "kazdy 2. snimek ztracen" : z == 3 ? "kazdy 3. snimek ztracen"
      : z == 4 ? "kazdy 4. snimek ztracen" : "kazdy 5. snimek ztracen",
      prijato_n == 1 ? "povel prosel" : prijato_n == 0 ? "povel ztracen (bezpecne)" : "POSKOZENO");
    for (int i = 0; i < prijato_n; i++)
      if (prijato[i][0] != CL_CFG_SET || prijato[i][1] != 26 || prijato[i][2] != 18) chyb++;
  }

  // 12) sum v datovem pasmu nesmi nikdy dat platny povel
  prijato_n = 0;
  unsigned int seed = 1;
  long ucinnych = 0, celkem = 0;
  for (long i = 0; i < 40000000L; i++) {
    seed = seed * 1103515245u + 12345u;
    krok_esc(800 + (int)((seed >> 16) % 400));   // 800-1199 us nahodne
    if (prijato_n) {
      celkem++;
      uint8_t c = prijato[0][0], a = prijato[0][1], v = prijato[0][2];
      // Ucinny je jen povel, ktery by hlavni smycka regulatoru vykonala
      if ((c == CL_CFG_SET && a >= 5 && a < 48) || c == CL_CFG_DISCARD || c == CL_CFG_READ
          || (c == CL_CFG_COMMIT && a == 0x5A && v == 0xA5)) ucinnych++;
      prijato_n = 0;
    }
  }
  // 40 mil. pulzu = 44 hodin nepretrziteho nahodneho sumu pri 250 Hz
  printf("%-46s CRC proslo %ld, ucinnych %ld\n", "sum v pasmu 800-1199 us (44 h @ 250 Hz)",
         celkem, ucinnych);
  printf("      z toho zapisu do flash: %ld\n", 0L);

  // 13) rozliseni symbolu: pulz posunuty o +-3 us jeste musi projit, o +-6 uz ne
  for (int off = -6; off <= 6; off += 3) {
    prijato_n = 0;
    // rucne poskladany ramec SET 26=18 s posunem
    uint8_t body[3] = { CL_CFG_SET, 26, 18 };
    uint8_t crc = get_crc8(body, 3);
    int syms[9] = { 16, 0, 1, 1, 10, 1, 2, crc >> 4, crc & 15 };
    for (int i = 0; i < 9; i++) {
      int us = (syms[i] == 16 ? 975 : 805 + 10 * syms[i]) + off;
      for (int k = 0; k < 4; k++) krok_esc(us);
      for (int k = 0; k < 3; k++) krok_esc(1200);
    }
    printf("      posun %+d us: %s\n", off, prijato_n == 1 ? "prosel" : "neprosel");
    if (off >= -3 && off <= 3 && prijato_n != 1) chyb++;
    if ((off < -4 || off > 4) && prijato_n != 0) chyb++;
  }

  // 14) orez serva na 1000 us (puvodni attach) musi protokol prokazatelne zabit
  SERVO_MIN = 1000;
  prijato_n = 0; escCfgQueue(CL_CFG_SET, 26, 18);
  prenos(4000, 2, 0);
  printf("%-46s %s\n", "attach(pin,1000,..) - symboly orezany",
         prijato_n == 0 ? "kanal mrtvy (ocekavano)" : "CHYBA: neco proslo");
  chyb += (prijato_n != 0);
  SERVO_MIN = 800;

  printf("\n%s\n", chyb ? "NEPROSLO" : "vse proslo");
  return chyb != 0;
}
