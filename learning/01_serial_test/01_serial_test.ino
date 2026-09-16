/*
  HUB-8735 Ultra / AmebaPro2
  Learning 01: Serial output test

  Board: ideasHatch:AmebaPro2:Ameba_HUB-8735_ultra
  Serial monitor baud rate: 115200
*/

unsigned long lastReportMs = 0;
unsigned long reportCount = 0;

void setup() {
  Serial.begin(115200);

  // Give the serial connection a short moment to become ready.
  delay(500);

  Serial.println();
  Serial.println("HUB-8735 Ultra serial test started");
  Serial.println("AmebaPro2 core: 4.0.15-Release");
}

void loop() {
  const unsigned long now = millis();

  if (now - lastReportMs >= 1000) {
    lastReportMs = now;
    reportCount++;

    Serial.print("running_ms=");
    Serial.print(now);
    Serial.print(" report_count=");
    Serial.println(reportCount);
  }
}

