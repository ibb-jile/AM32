#!/bin/bash
# Prozene odesilatel z casovace proti dekoderu z regulatoru.
#
# Obe strany se VYTAHUJI ZE ZDROJU, ne prepisuji. Kdyz se kodovani na jedne
# strane zmeni a na druhe ne, test spadne - to je jeho jediny ucel. Hardware
# k tomu netreba.
set -eu
cd "$(dirname "$0")"
AM32="${AM32:-../..}"
TIMER="${TIMER:-$HOME/Documents/Arduino/us-timer/active-timer/active-timer.ino}"

[ -f "$TIMER" ] || { echo "nenalezen casovac: $TIMER (nastav TIMER=...)"; exit 1; }

# dekoder: blok #ifdef CL_SERVO_CONFIG az po prvni #endif
awk '/^#ifdef CL_SERVO_CONFIG$/{f=1;next} f&&/^#endif$/{exit} f' "$AM32/Src/signal.c" \
  | sed '/^extern uint8_t get_crc8/d' > dec.inc

# odesilatel: od konstant symbolu po escVystupObsazen()
A=$(grep -n '^const int ESC_SYM_BASE' "$TIMER" | cut -d: -f1)
# Konec bloku je prvni radek za odesilatelem, at uz je to most do bootloaderu
# nebo primo escVystupObsazen(). Bez toho se do testu pritahne i kod mostu,
# ktery na Arduino knihovnach zavisi a mimo desku se neprelozi.
B=$(awk -v a="$A" 'NR>a && (/^\/\/ ---------- Most do bootloaderu/ || /^bool escVystupObsazen\(\)/) {print NR; exit}' "$TIMER")
sed -n "${A},$((B - 1))p" "$TIMER" > snd.inc

[ -s dec.inc ] && [ -s snd.inc ] || { echo "extrakce selhala"; exit 1; }
g++ -O2 -o test test.cpp
./test
