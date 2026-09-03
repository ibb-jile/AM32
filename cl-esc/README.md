# CL-ESC — regulátor pro upoutané modely

Konfigurace **DYS Aria 70A** (STM32F051K8 + Fortior FD6288Q) pro upoutané modely
letadel. Patří k targetu `CL_ARIA_F051` v [`Inc/targets.h`](../Inc/targets.h).

Deska se prodává s BLHeli_32. Postup převodu na AM32 je popsaný v
[AM32 wiki](https://wiki.am32.ca/guides/Hacking-Guide.html); použitý bootloader je
`AM32_F051_BOOTLOADER_PA2_V19.hex` z [releasu v19.0.0](https://github.com/am32-firmware/AM32-bootloader/releases).
Vstup je na **PA2**, telemetrie na **PB6** (na desce ploška `OE`), SWD na ploškách
`C` = SWCLK a `D` = SWDIO.

## `CL_eeprom_v4.bin`

Výchozí obraz z konfigurátoru (`eeprom-defaults/DEFAULT/v4.bin`) není pro upoutané
modely použitelný. Tenhle se od něj liší v šesti bajtech:

| Bajt | Pole | Výchozí | Zde | Proč |
|---|---|---|---|---|
| 7 | `disable_stick_calibration` | 0 | **1** | viz níž — jinak ESC vleze do kalibrace za letu |
| 31 | `telemetry_on_interval` | 0 | **1** | telemetrie po PB6, rámec každých ~60 ms |
| 32 | `servo.low_threshold` | 128 (1006 µs) | **235 (1220 µs)** | klid časovače je 1200 µs; s výchozím prahem by to ESC četlo jako ~21 % plynu |
| 33 | `servo.high_threshold` | 128 (2006 µs) | **125 (2000 µs)** | maximum časovače je 2000 µs |
| 36 | `low_voltage_cut_off` | 0 | **1** | podpětní ochrana na článek, počet článků se zjistí při armování |
| 46 | `input_type` | 1 = DSHOT | **2 = SERVO_IN** | výchozí obraz vnucuje DShot a se servo pulzy ESC vůbec nearmuje |

Přepočet prahů: `low = 750 + bajt*2`, `high = 1750 + bajt*2` (`Src/main.c:675`).

### Past: kalibrace koncových bodů za letu

AM32 spouští kalibraci, když plyn drží **nad 1500 µs ustáleně** (odchylka < 50) po
~50 cyklů (`Src/signal.c:181`). U multikoptéry se plyn pořád mění, takže na to nikdo
nenarazí. Upoutaný model ale letí celou minutu na konstantním plynu — přesně ta
podmínka. Bez `disable_stick_calibration = 1` by regulátor uprostřed letu přepsal
koncové body.

### Past: prahy potřebují rezervu

Práh nastavený přesně na klidovou hodnotu časovače nestačí. AM32 nuluje jen
`servorawinput <= 48`, což při prahu 1200 µs odpovídá ~1200,4 µs — a generátor
serva na ESP32 zaokrouhlí pulz podle opakovací frekvence klidně o 3 µs jinam. ESC
pak čte 2 % plynu a **vůbec nearmuje**. Proto je práh 1220 µs, tedy 17 µs rezervy.
Při změně opakovací frekvence časovače tohle překontroluj.

## Zápis

```sh
openocd -f interface/cmsis-dap.cfg -c "transport select swd" \
        -c "adapter speed 2000" -f target/stm32f0x.cfg \
        -c "init" -c "halt" \
        -c "program cl-esc/CL_eeprom_v4.bin verify 0x08007C00" \
        -c "reset run" -c "shutdown"
```

Kontrola po zápisu (adresy vytáhni `nm` z elfu): `armed = 1`, `servoPwm = 1`,
`dshot = 0`, `adjusted_input = 0` při klidovém pulzu, `cell_count` odpovídá baterii.

> **Pozor:** mass erase při převodu z BLHeli_32 smaže i tuhle stránku. Bootloader
> kontroluje první bajt EEPROM a když tam není `0x01`, **odmítne skočit do
> aplikace** (`AM32-bootloader/bootloader/main.c:429`). Vypadá to jako špatně
> nahraný firmware, ale chybí jen tenhle soubor.

## Kalibrace konstant v targetu

Obě konstanty v `CL_ARIA_F051` jsou změřené na kuse č. 1, ne převzaté:

- `TARGET_VOLTAGE_DIVIDER 113` — 1062 mV na PA6 při 11,982 V na ploškách `V+`/`V-`.
  Firmware pak hlásí 11,91–12,01 V.
- `MILLIVOLT_PER_AMP 16` — proti laboratornímu zdroji při 17 000 1/min bez vrtule.
  V klidu je syrová hodnota ADC nulová, takže charakteristika prochází počátkem
  a `CURRENT_OFFSET` zůstává 0. Ověření dalo 1,153 A proti 1,14 A ze zdroje (+1,1 %).

**Proud je kalibrovaný jen okolo 1 A**, což je u 70A regulátoru úplný začátek
rozsahu. Před provozním nasazením ho přeměř při zátěži blíž skutečnému odběru.
