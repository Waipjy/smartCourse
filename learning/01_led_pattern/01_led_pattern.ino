/*
  HUB-8735 Ultra / AmebaPro2
  Learning 02: Blue and green LED pattern

  Verified against the HUB-8735 Ultra schematic and AmebaPro2 variant:
    LED_B = IO26 = AMB_D26 = PF_9
    LED_G = IO25 = AMB_D25 = PE_6

  The LEDs are driven through low-side MOSFETs, so HIGH turns an LED on.
*/

const uint8_t BLUE = 0x01;
const uint8_t GREEN = 0x02;

struct PatternStep {
  uint8_t leds;
  unsigned long durationMs;
};

// A small "status light" animation: blue, green, both, then a pause.
const PatternStep pattern[] = {
  {BLUE, 180},
  {0, 100},
  {GREEN, 180},
  {0, 100},
  {BLUE | GREEN, 120},
  {0, 250},
  {GREEN, 90},
  {0, 90},
  {BLUE, 90},
  {0, 700}
};

const size_t patternLength = sizeof(pattern) / sizeof(pattern[0]);
size_t currentStep = 0;
unsigned long stepStartedMs = 0;

void applyLeds(uint8_t leds) {
  digitalWrite(LED_B, (leds & BLUE) ? HIGH : LOW);
  digitalWrite(LED_G, (leds & GREEN) ? HIGH : LOW);
}

void setup() {
  pinMode(LED_B, OUTPUT);
  pinMode(LED_G, OUTPUT);
  applyLeds(pattern[0].leds);

  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("HUB-8735 Ultra blue/green LED pattern");
  Serial.println("Blue LED=IO26/PF_9, Green LED=IO25/PE_6");
}

void loop() {
  const unsigned long now = millis();

  if (now - stepStartedMs >= pattern[currentStep].durationMs) {
    currentStep = (currentStep + 1) % patternLength;
    stepStartedMs = now;
    applyLeds(pattern[currentStep].leds);
  }
}
