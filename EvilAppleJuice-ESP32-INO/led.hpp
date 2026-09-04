enum LEDMode { OFF = 0, FLASH = 1, ON = 2 };

LEDMode stateTable[9][2] = {
  {OFF,   OFF},   // 0
  {OFF,   FLASH}, // 1
  {OFF,   ON},    // 2
  {FLASH, OFF},   // 3
  {FLASH, FLASH}, // 4
  {FLASH, ON},    // 5
  {ON,    OFF},   // 6
  {ON,    FLASH}, // 7 
  {ON,    ON}     // 8
};

enum LEDState {
  LEFT_OFF_RIGHT_OFF = 0,
  LEFT_OFF_RIGHT_FLASH = 1,
  LEFT_OFF_RIGHT_ON = 2,
  LEFT_FLASH_RIGHT_OFF = 3,
  LEFT_FLASH_RIGHT_FLASH = 4,
  LEFT_FLASH_RIGHT_ON = 5,
  LEFT_ON_RIGHT_OFF = 6,
  LEFT_ON_RIGHT_FLASH = 7,
  LEFT_ON_RIGHT_ON = 8
};