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

## Nastavení regulátoru z časovače

Nastavení jako `motor_kv` nebo počet pólů se dá standardně změnit jen
programátorem nebo konfigurátorem přes bootloader — tedy s modelem na stole
a rozebraným drakem. Právě proto v regulátoru zůstalo výchozí `motor_kv`
odpovídající 2220 KV, ačkoli motor má 740 KV, a strop plynu byl při 6 000 ot/min
sražený na 51 %. Motor nešlo roztočit ani pulzem 2000 µs a přišlo se na to až
po letovém pokusu.

Fork proto umí nastavení číst i zapisovat **po dvou vodičích, které v modelu
už jsou**. Nic se nepřepájí a nepotřebuje se ani bootloader, ani odpojení
baterie:

| směr | vodič | co po něm jde |
|---|---|---|
| ESC → časovač | telemetrie (PB6) | 48 bajtů EEPROM + CRC8 v info paketu (`makeInfoPacket`) |
| časovač → ESC | signál (PA2) | povel zakódovaný do délky servo pulzů |

### Kódování povelu

Každý servo snímek nese jeden symbol podle délky pulzu:

| délka pulzu | význam |
|---|---|
| 805–955 µs, krok 10 µs | datový půlbajt 0–15 |
| 975 µs | SYNC, začátek rámce |
| cokoli jiného | mezera |

Rámec je `SYNC + 8 půlbajtů` = `cmd, addr, val, crc8`. Symbol se přijme až po
**dvou shodných snímcích**, časovač ho drží 16 ms a odděluje 12 ms klidového
pulzu — bez té mezery by dva stejné symboly za sebou splynuly v jeden. Jeden
povel trvá 252 ms.

Všechny symboly leží pod prahem stopu i pod tónovými pásmy (ta začínají
na 1000 µs), takže je regulátor po celou dobu čte jako nulový plyn a s pípáním
se nekříží.

Povely (`CL_CFG_*` v `Inc/targets.h`):

| kód | povel |
|---|---|
| 0x01 | zapsat bajt EEPROM do RAM |
| 0x02 | uložit do flash a znovu načíst; `addr`/`val` musí být `0x5A`/`0xA5` |
| 0x03 | zahodit neuložené změny |
| 0x04 | poslat info paket |

Uložení volá `saveEEpromSettings()` a hned `loadEEpromSettings()`, takže se
odvozené meze přepočítají a **nastavení platí okamžitě** — power-cycle netřeba.

### Čím je to jištěné

- Symbol se přijme až po dvou shodných snímcích, takže jeden rušený pulz nic nezmění.
- Rámec chrání CRC8 a kontrola rozsahu povelu.
- Za běhu motoru se nepřijme nic (`if (running) return;`).
- Zapsat lze jen bajty **5–47**. Bajt 0 je branou bootloaderu a 1–4 nesou verzi;
  přepsat je znamená odstavit desku tak, že ji časovač už nespraví.
- Zápis do flash navíc vyžaduje dvoubajtový klíč `0x5A`/`0xA5`.
- Práce s flash se nedělá v přerušení od vstupního zachytu, ale až v hlavní
  smyčce (`Src/main.c`) — mazání stránky trvá desítky ms a v přerušení by
  shodilo měření délky pulzu i komutaci.

### Past: `attach()` na časovači ořezává

`ESP32Servo::writeTicks()` ořezává na meze zadané v `attach()`. Původní
`attach(pin, 1000, 2000)` by všechny symboly srazil na 1000 µs — a to je navíc
tónové pásmo, takže by regulátor místo nastavení **pípal**. Dolní mez proto
musí být pod 805 µs.

### Test bez hardwaru

`test-protokol/run.sh` vytáhne dekodér z `Src/signal.c` a odesílatel
z `active-timer.ino` (nepřepisuje je) a prožene je proti sobě: běžné povely,
ztráta snímků, šum v datovém pásmu, tolerance délky pulzu, ořez serva.
Když se kódování na jedné straně změní a na druhé ne, test spadne.

```bash
./test-protokol/run.sh
```

## Nastavení bez programátoru — tovární bootloader

Kanál popsaný výš je součástí *našeho* firmwaru, takže se do regulátoru musí
napřed jednou dostat programátorem. Nastavení ale jde měnit i bez něj a bez
ohledu na to, jaký firmware v regulátoru běží: **tovární bootloader AM32 mluví
po signálovém vodiči**, na kterém časovač už visí.

Časovač k tomu dělá jen most (sériový příkaz `bl`), protokol běží na počítači
v `active-timer/tools/am32bl.py`. Baterie se neodpojuje — most drží signál
nahoře, aplikace se po ~2 s sama resetuje a bootloader v ní pak nezůstane.

**Ověřeno na hardwaru (září 2026):** přečteno deviceInfo `471`, přečten a zpětně
ověřen celý blok 192 bajtů, přepsán `motor_kv` z 55 (2220 KV) na 18 (740 KV).

Zápis jde vždycky po celém bloku 192 bajtů na `0x7C00` — `save_flash_nolib`
maže stránku, když je adresa dělitelná 1024, takže částečný zápis by zbytek
nastavení smazal.

## Pohon — ověřeno měřením naprázdno (září 2026)

Turnigy Aerodrive SK3 4240-740KV, **6S**, 14 pólů. Měřeno bez vrtule přes
sériový povel `pulse` v časovači:

| pulz | otáčky | | pulz | otáčky |
|---|---|---|---|---|
| 1300 µs | 1 985 | | 1700 µs | 10 200 |
| 1400 µs | 4 114 | | 1800 µs | 12 328 |
| 1500 µs | 6 157 | | 1900 µs | 14 400 |
| 1600 µs | 8 100 | | 2000 µs | 16 414 |

Sklon 20,8 ot/µs, protažením k nulovým otáčkám vychází 1 211 µs — prakticky
přesně práh stopu 1 220 µs, takže mapování plynu sedí.

**Počet pólů 14 je tím potvrzený**, byl to do té doby odhad. 740 KV × 22,2 V dá
naprázdno 16 428 ot/min a naměřilo se 16 414, tedy rozdíl 0,1 %. Kdyby měl motor
28 pólů, byly by skutečné otáčky poloviční a KV by nesedělo ani zdaleka.

**Tohle měření ale neověřuje opravu `motor_kv`.** Strop plynu zasahuje jen při
nízkých otáčkách a vysokém plynu, tedy pod zátěží; naprázdno se motor vždycky
roztočí nad strop. Spočítáno: v žádném z osmi kroků by strop nezasáhl ani se
starým nastavením 2220 KV. Ověřit ho půjde až s vrtulí.

**K napětí:** telemetrie hlásí ~11,65 V i na 6S. Není to měření — hodnota při
proudových špičkách *stoupá* (11,65 → 11,94 V při 2,2 A), zatímco baterie pod
zátěží klesá. Potvrzuje to původní závěr, že PA6 s napětím baterie nesouvisí.

## Test s vrtulí — ověření opravy `motor_kv` (září 2026)

Model uvázaný, 6S, krokovaný plyn po 3 s (`pulse` v časovači, jištěno na 60 A /
80 °C / ztrátu telemetrie). Teplota regulátoru vystoupala z 38 na 50 °C, proud
nepřesáhl 6,6 A.

| pulz | žádaný plyn | otáčky | k_erpm | strop NOVÝ (kv=18) | strop STARÝ (kv=55) |
|---|---|---|---|---|---|
| 1300 µs | 10 % | 1 700 | 11 | 46,5 % | 22,2 % |
| 1400 µs | 23 % | 3 385 | 23 | 80,5 % | 33,1 % |
| 1500 µs | 36 % | 4 900 | 34 | **100 %** | 44,0 % |
| 1600 µs | 49 % | 6 357 | 44 | 100 % | 53,8 % |
| 1700 µs | 62 % | 7 500 | 52 | 100 % | 61,1 % ← škrtí |
| 1800 µs | 74 % | 8 485 | 59 | 100 % | 67,2 % ← škrtí |
| 1900 µs | 87 % | 9 542 | 67 | 100 % | 74,9 % ← škrtí |
| 2000 µs | 100 % | 10 614 | 74 | 100 % | 82,7 % ← škrtí |

Od 1500 µs (4 900 ot/min) je strop plně otevřený a v celém zbytku rozsahu už
zasáhnout nemůže.

**Proč se starým nastavením nešlo přidat.** Škrcení srazí otáčky, nižší otáčky
strop ještě přivřou a to škrtí dál — je to samo se posilující smyčka. Ustálí se
tam, kde se žádaný plyn rovná stropu, a to je podle naměřené křivky **61,5 %,
tedy 7 500 ot/min**. Přesně tolik by model dal při 1700 µs i při 2000 µs; víc už
z něj nešlo dostat ničím. Po opravě dá plný plyn 10 614 ot/min, tedy **o 41 % víc**.

Statické otáčky jsou spodní odhad — za letu se vrtule odlehčí a poroste.
