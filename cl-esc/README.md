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

## Pípání motorem na povel z časovače

`CL_SERVO_TONES` v targetu zapíná `clServoTone()` v [`Src/signal.c`](../Src/signal.c).
Časovač jede v pásmu 1200–2000 µs, takže vše pod prahem stopu je volné a použité
jako povelový kanál:

| Puls | Povel | Zvuk |
|---|---|---|
| 1120–1160 µs | tón 1 — odpočet spuštěn | `playDefaultTone()`, ~300 ms |
| 1060–1100 µs | tón 2 — poslední sekundy před startem | `playChangedTone()`, ~300 ms |
| 1000–1040 µs | tón 3 — chyba časovače | `playBeaconTune3()`, ~600 ms (melodie) |

Mezi pásmy jsou mezery 20 µs, protože generátor serva na ESP32 zaokrouhluje pulz
o jednotky mikrosekund a s opakovací frekvencí se to mění. Povel platí až po
`CL_TONE_DEBOUNCE` snímcích v pásmu (při 250 Hz je 20 snímků 80 ms) a spustí se jen
jednou při vstupu do pásma, takže držení pásma nepípá dokola.

Všechna pásma leží pod prahem stopu, takže je ESC čte jako nulový plyn a armování
to neruší.

> **Bezpečnost:** pípání roztáčí cívky motoru, proto se povel ignoruje, když motor
> běží (`!running`). Za letu ho nejde vyvolat ani chybou v časovači. Časovač si to
> hlídá podruhé na své straně.

Bez DShotu jinak pípnout nejde — `play_tone_flag` nastavuje v původním AM32 jedině
`Src/dshot.c` (beacon povely 1–5) a ze servo vstupu k němu nevede žádná cesta.

## Stavová RGB LED

`CL_STATUS_LED` zapíná vlastní signalizaci v [`Src/main.c`](../Src/main.c). Deska má
RGB LED se **společnou anodou** na 3,3 V a katodami přes odpory do procesoru, takže
barva svítí při **nule** na pinu. Piny i polarita jsou změřené na kuse č. 1:

| Barva | Pin |
|---|---|
| červená | **PA15** (má na desce pull-up) |
| zelená | **PB3** |
| modrá | **PB4** |

Signalizované stavy:

| Barva | Stav |
|---|---|
| červená | nearmováno / bez signálu |
| zelená | armováno, připraveno |
| modrá | motor běží |
| červená blikající ~2 Hz | podpěťová ochrana vypnula — nutný power-cycle |

Vlastní `USE_RGB_LED` z AM32 nepoužíváme, protože **nefunguje**:
`setIndividualRGBLed()` rozsvěcuje až při hodnotě `> 1`, ale všechna volání v
`main.c` posílají 0/1 (`setIndividualRGBLed(1,0,0)`), takže vždy spadnou do větve
„zhasnout". Rozsvítí se jen na okamžik po `LED_GPIO_init()`, kde jsou piny v nule.

Jako časová základna blikání slouží `ledcounter` z `tenKhzRoutine()` — u nás ho nic
nenuluje, protože větev `USE_CUSTOM_LED` nepoužíváme.

## Kalibrace konstant v targetu

Obě konstanty v `CL_ARIA_F051` jsou změřené na kuse č. 1, ne převzaté:

- `MILLIVOLT_PER_AMP 16` — proti laboratornímu zdroji při 17 000 1/min bez vrtule.
  V klidu je syrová hodnota ADC nulová, takže charakteristika prochází počátkem
  a `CURRENT_OFFSET` zůstává 0. Ověření dalo 1,153 A proti 1,14 A ze zdroje (+1,1 %).

**Proud je kalibrovaný jen okolo 1 A**, což je u 70A regulátoru úplný začátek
rozsahu. Před provozním nasazením ho přeměř při zátěži blíž skutečnému odběru.

## Napětí baterie se na této desce měřit nedá

`ARIA_RAMP_F051` uvádí dělič na PA6, ale tady PA6 s baterií nesouvisí:

| Změna | `ADC_raw_volts` |
|---|---|
| 12,0 V → 5,7 V na `V+` | 1320 → 1320 |
| ohřev, jádro 33 → 38 °C | 1320 → 1320 |
| vnitřní pull-up i pull-down 40 kΩ | 1320 → 1320 |

Je to nízkoimpedanční pevný zdroj 1,064 V. Hodnota 1320 ze 4095 je přesně **0,3224
reference ADC**, a protože referencí je tentýž 3,3V rail, ze kterého procesor žije,
je to **dělič z 3,3 V** — signál i reference se škálují spolu, takže poměr je
neměnný za všech okolností.

Volný ADC pin už nezbývá: PA0/PA4/PA5 jsou komparátory fází, PA2 signál, PA3 proud,
PA7 spodní gate, PA1 je natvrdo na zemi (5 mV i proti pull-upu 40 kΩ).

**Důsledky:**

- `TARGET_VOLTAGE_DIVIDER` se nedefinuje a podpěťová ochrana je v EEPROM vypnutá.
  Zapnutá by se tvářila jako funkční ochrana, která nikdy nezabere.
- `battery_voltage` zůstává na konstantních ~11,9 V a **tuto hodnotu nelze použít**.
  Leze i do telemetrie, takže sloupec `esc_v` v záznamu letu je pro tuhle desku
  neplatný.
- Konstantu **záměrně nenulujeme**: `battery_voltage` řídí i rychlost změny střídy
  (`map(battery_voltage, 800, 2200, 10, 1)`, `Src/main.c:1517`). Současná hodnota
  dává rozumných 7, nula by dala 15 a motor by měnil výkon dvakrát rychleji.

Pozor na past, na kterou jsme naletěli: kalibrace v **jediném** pracovním bodě dala
věrohodnou konstantu 113, odpovídající běžnému děliči 100k/10k. Ověření pak souhlasilo,
protože porovnávalo hodnotu s tím, na co byla předtím nastavena. Dvoubodové měření
při dvou různých napětích to odhalí okamžitě.
