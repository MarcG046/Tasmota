/*
  xdrv_61_Multiplexer.ino - 

  Copyright (C) 2022 Marc Geilen

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*

A driver for an extension with an analog multiplexer in front of the ADC
- 4 pins to select the 1 out of 16 inputs
- an enable pin (active low)
- 1 pin as the ADC input  ADC support for ESP8266 GPIO17 (=PIN_A0)

Every second the driver selects another input through the multiplexer and takes a reading form the ADC
It maintains values for all inputs to the multiplexer and offers them simultaneously as its sensor state

*/

#ifdef USE_MULTIPLEXED_ADC

#define XDRV_61           61

// copied from xsns_02_analog.ino
#ifdef ESP8266
#define MUL_ANALOG_RESOLUTION             10               // 12 = 4095, 11 = 2047, 10 = 1023
#endif  // ESP8266
#ifdef ESP32
#undef MUL_ANALOG_RESOLUTION
#define MUL_ANALOG_RESOLUTION             12               // 12 = 4095, 11 = 2047, 10 = 1023
#endif  // ESP32

struct Multiplexer {
  uint8_t pinSEL0;
  uint8_t pinSEL1;
  uint8_t pinSEL2;
  uint8_t pinSEL3;
  uint8_t pinENABLE;
  uint8_t pinADC;
  bool connected = false;
  uint8_t activeInput = 0;
  uint16_t values[16];
} *Multiplexer = nullptr;


void MultiplexerAdcConfigurePin(uint8_t pin, uint8_t value = 0) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, value);
}


void MultiplexerInit(void) {
  // activate if all pins have been defined
  if (PinUsed(GPIO_MULTIPLEXER_SEL0) && 
      PinUsed(GPIO_MULTIPLEXER_SEL1) && 
      PinUsed(GPIO_MULTIPLEXER_SEL2) && 
      PinUsed(GPIO_MULTIPLEXER_SEL3) && 
      PinUsed(GPIO_MULTIPLEXER_EN) && 
      PinUsed(GPIO_MULTIPLEXER_ADC)
      ) {
    Multiplexer = (struct Multiplexer*)calloc(1, sizeof(struct Multiplexer));
    if (Multiplexer) {
      Multiplexer->pinSEL0 = Pin(GPIO_MULTIPLEXER_SEL0);
      Multiplexer->pinSEL1 = Pin(GPIO_MULTIPLEXER_SEL1);
      Multiplexer->pinSEL2 = Pin(GPIO_MULTIPLEXER_SEL2);
      Multiplexer->pinSEL3 = Pin(GPIO_MULTIPLEXER_SEL3);
      Multiplexer->pinENABLE = Pin(GPIO_MULTIPLEXER_EN);
      Multiplexer->pinADC = Pin(GPIO_MULTIPLEXER_ADC);
      
      MultiplexerAdcConfigurePin(Multiplexer->pinSEL0);
      MultiplexerAdcConfigurePin(Multiplexer->pinSEL1);
      MultiplexerAdcConfigurePin(Multiplexer->pinSEL2);
      MultiplexerAdcConfigurePin(Multiplexer->pinSEL3);
      MultiplexerAdcConfigurePin(Multiplexer->pinENABLE);

      // copied from xsns_02_analog.ino
#ifdef ESP32
      analogSetClockDiv(1);               // Default 1
#if CONFIG_IDF_TARGET_ESP32
      analogSetWidth(MUL_ANALOG_RESOLUTION);  // Default 12 bits (0 - 4095)
#endif  // CONFIG_IDF_TARGET_ESP32
      analogSetAttenuation(ADC_11db);     // Default 11db
#endif

      // initialize readings to -1 (no reading)
      for(uint8_t i=0; i<16; i++){
        Multiplexer->values[i] = -1;
      }
      Multiplexer->activeInput = 0;
      Multiplexer->connected = true;
      AddLog(LOG_LEVEL_DEBUG, PSTR("Multiplexed ADC active"));
    }
  }
}

void MultiplexerSelectInput(uint8_t n) {
  if (Multiplexer && Multiplexer->connected == true) {
    digitalWrite(Multiplexer->pinSEL0, bitRead(n,0));
    digitalWrite(Multiplexer->pinSEL1, bitRead(n,1));
    digitalWrite(Multiplexer->pinSEL2, bitRead(n,2));
    digitalWrite(Multiplexer->pinSEL3, bitRead(n,3));
    digitalWrite(Multiplexer->pinENABLE, 0);
  }
}

// take an averaged analog reading from multiple samples
uint16_t MAdcRead(uint32_t pin, uint32_t factor) {
  uint32_t samples = 1 << factor;
  uint32_t analog = 0;
  for (uint32_t i = 0; i < samples; i++) {
    analog += analogRead(pin);
    delay(1);
  }
  analog >>= factor;
  return analog;
}


// called every second, switch to next input and take reading
void MAdcEverySecond() {
  if (Multiplexer && Multiplexer->connected == true) {
    Multiplexer->activeInput += 1;
    if (Multiplexer->activeInput > 15) Multiplexer->activeInput = 0;
    MultiplexerSelectInput(Multiplexer->activeInput);
    // maybe the switch needs some time ?
    delay(1);
    Multiplexer->values[Multiplexer->activeInput] = MAdcRead(Multiplexer->pinADC, 5);
  }
}


// helper function to generate JSON
void MAdcShowJSONContinuation(bool *jsonflg) {
  if (*jsonflg) {
    ResponseAppend_P(PSTR(","));
  } else {
    ResponseAppend_P(PSTR(",\"ANALOG\":{"));
    *jsonflg = true;
  }
}


// make JSON representation of the state
void MAdcShowJSON() {
  // offset copied from xsns_02_analog.ino, not sure why it is done
  uint32_t offset = 0;
#ifdef ESP32
    offset = 1;
#endif

  bool jsonflg = false;
  for (uint32_t idx = 0; idx < 16; idx++) {
    if (Multiplexer->values[idx] != -1) {
      MAdcShowJSONContinuation(&jsonflg);
      ResponseAppend_P(PSTR("\"A%d\":%d"), idx + offset, Multiplexer->values[idx]);
    }
  }
  if (jsonflg) {
    ResponseJsonEnd(); // "}"
  }
}

#ifdef USE_WEBSERVER
// make web representation of the state
void MAdcShowWeb() {
  bool domo_flag[ADC_END] = { false };
  uint32_t offset = 0;

  for (uint32_t idx = 0; idx < 16; idx++) {
#ifdef ESP32
    offset = 1;
#endif
    WSContentSend_PD(HTTP_SNS_ANALOG, "", idx + offset, Multiplexer->values[idx]);
  }
}
#endif

/*********************************************************************************************\
 * Interface
\*********************************************************************************************/

bool Xdrv61(uint8_t function) {
  bool result = false;

  if (FUNC_PRE_INIT == function) {
    MultiplexerInit();
    } else if (Multiplexer) {
      switch (function) {
        case FUNC_JSON_APPEND: // report data to JSON
          MAdcShowJSON();
          break;        

#ifdef USE_WEBSERVER
        case FUNC_WEB_SENSOR: // report data to web interface
          MAdcShowWeb();
          break;
#endif  // USE_WEBSERVER

        case FUNC_EVERY_SECOND:
          MAdcEverySecond();
          break;
      }
    }
  return result;
}

#endif  // USE_MULTIPLEXED_ADC
