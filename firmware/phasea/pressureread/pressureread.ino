#include <SparkFun_MS5803_I2C.h>
#include <Wire.h>

MS5803 sensor(ADDRESS_HIGH);   // 스캐너 결과 0x76 → ADDRESS_HIGH 확정

void setup() {
  Serial.begin(115200);
  delay(1000);
  Wire.begin();                // I2C 초기화 (SDA=GPIO21, SCL=GPIO22)
  Serial.println("=== Phase A: pressureread ===");
  sensor.reset();
  delay(10);                   // PROM 보정계수 안정화
  sensor.begin();              // 보정계수 로드
  delay(10);
}

void loop() {
  float temp_c        = sensor.getTemperature(CELSIUS, ADC_512);
  float pressure_mbar = sensor.getPressure(ADC_4096);

  Serial.print("Temp: ");
  Serial.print(temp_c);
  Serial.print(" C   Pressure: ");
  Serial.print(pressure_mbar);
  Serial.println(" mbar");
  delay(500);
}
