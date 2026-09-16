/*
  HUB-8735 Ultra traffic light demo
  Breadboard LEDs:
    Green  = IO24
    Yellow = IO23
    Red    = IO22

  Wiring assumption: GPIO -> resistor -> LED -> GND.
  Therefore HIGH turns an LED on.
*/

const uint8_t GREEN_LED = 24;
const uint8_t YELLOW_LED = 23;
const uint8_t RED_LED = 22;

enum LightState {
  GREEN_STATE,
  YELLOW_STATE,
  RED_STATE
};

LightState state = GREEN_STATE;
unsigned long stateStartedMs = 0;

void setLights(bool green, bool yellow, bool red) {
  digitalWrite(GREEN_LED, green ? HIGH : LOW);
  digitalWrite(YELLOW_LED, yellow ? HIGH : LOW);
  digitalWrite(RED_LED, red ? HIGH : LOW);
}

void enterState(LightState nextState) {
  state = nextState;
  stateStartedMs = millis();

  switch (state) {
    case GREEN_STATE:
      setLights(true, false, false);
      Serial.println("GREEN - 3 seconds");
      break;
    case YELLOW_STATE:
      setLights(false, true, false);
      Serial.println("YELLOW - 1 second");
      break;
    case RED_STATE:
      setLights(false, false, true);
      Serial.println("RED - 5 seconds");
      break;
  }
}

void setup() {
  pinMode(GREEN_LED, OUTPUT);
  pinMode(YELLOW_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);

  Serial.begin(115200);
  setLights(false, false, false);
  enterState(GREEN_STATE);
}

void loop() {
  const unsigned long elapsed = millis() - stateStartedMs;

  if (state == GREEN_STATE && elapsed >= 3000) {
    enterState(YELLOW_STATE);
  } else if (state == YELLOW_STATE && elapsed >= 1000) {
    enterState(RED_STATE);
  } else if (state == RED_STATE && elapsed >= 5000) {
    enterState(GREEN_STATE);
  }
}

