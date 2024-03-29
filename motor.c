
#include <xc.h>
#include "types.h"
#include "motor.h"
#include "i2c.h"
#include "state.h"
#include "pins.h"
#include "clock.h"
#include "move.h"


uint16 speedDbg[5][4];
uint8  speedDbgPtr = 0;


// globals for use in main event loop
uint8  motorIdx;
struct motorState      *ms;
struct motorSettings   *sv;

volatile unsigned char *motorPort[NUM_MOTORS][4] = {
  {&z0PORT, &z1PORT, &z2PORT, &z3PORT},
  {&l0PORT, &l1PORT, &l2PORT, &l3PORT},
  {&p0PORT, &p1PORT, &p2PORT, &p3PORT},
};

uint8 motorMask[NUM_MOTORS][4] = {
  {z0BIT, z1BIT, z2BIT, z3BIT},
  {l0BIT, l1BIT, l2BIT, l3BIT},
  {p0BIT, p1BIT, p2BIT, p3BIT},
};

void setMotorPin(uint8 mot, uint8 pin, bool on) {
#ifdef  debug
  if(mot == 1 && pin == 0) return;
#endif
  if(on) *motorPort[mot][pin] |=  motorMask[mot][pin];
  else   *motorPort[mot][pin] &= ~motorMask[mot][pin];
}

// default startup values
// must match settingsStruct
uint16 settingsInit[NUM_SETTING_WORDS] = {
      1,  // accelIdx accelleration rate: 1 mm/sec/sec
   6400,  // max speed: 80 mm/sec
   1600,  // jerk speed  20 mm/sec
      0,  // homePos: home value used by command
};
// estimated decell distance by ms->speed
// wild guess for now
uint16 decellTable[][2] = {
  {150*STEPS_MM, 32*STEPS_MM},
  {100*STEPS_MM, 16*STEPS_MM},
  { 80*STEPS_MM,  8*STEPS_MM},
  { 60*STEPS_MM,  4*STEPS_MM},
  { 40*STEPS_MM,  2*STEPS_MM},
  { 20*STEPS_MM,  1*STEPS_MM},   
};

bool withinDecellDist() {
  int16 distRemaining = (ms->targetPos - ms->curPos);
  ms->targetDir = 1;
  if(distRemaining < 0) {
    ms->targetDir = 0;
    distRemaining = -distRemaining;
  }
  if(ms->targetDir != ms->dir) return true;
  
  for(uint8 i = 0; i < sizeof(decellTable)/2; i++) {
    if(ms->speed >= decellTable[i][0] &&
       distRemaining <= decellTable[i][1]) {
      return true;
    }
  }
  return false;
}

void motorInit() {
  for(uint8 motIdx=0; motIdx < NUM_MOTORS; motIdx++) {
    struct motorState *p = &mState[motIdx];
    p->curPos          = 0;  // 1/80 mm
    p->dir             = 1;  // 1 => forward
    p->phase           = 0;
    p->targetPos       = 0;  // 1/80 mm
    p->speed           = 1;  // 1/80 mm/sec
    p->targetSpeed     = 1;  // 1/80 mm/sec
    
    for(uint8 i = 0; i < NUM_SETTING_WORDS; i++) {
       mSet[motIdx].reg[i] = settingsInit[i];
    }
  }
  // motors turned off
  z0LAT  = z1LAT  = z2LAT  = z3LAT  = 0;
  l0LAT  = l1LAT  = l2LAT  = l3LAT  = 0;
  p0LAT  = p1LAT  = p2LAT  = p3LAT  = 0;
  
  z0TRIS = z1TRIS = z2TRIS = z3TRIS = 0;
  l0TRIS = l1TRIS = l2TRIS = l3TRIS = 0;
  p0TRIS = p1TRIS = p2TRIS = p3TRIS = 0;
  
#ifdef debug 
  dbg0
  dbgTRIS = 0;
#endif
}

void setMotorSettings(uint8 numWords) {
  if(numWords > NUM_SETTING_WORDS) numWords = NUM_SETTING_WORDS;
  for(uint8 i = 0; i < numWords; i++) {
    mSet[motorIdx].reg[i] = (i2cRecvBytes[motorIdx][2*i + 2] << 8) | 
                             i2cRecvBytes[motorIdx][2*i + 3];
  }
  ms->haveSettings = true;
}

void stopStepping() {
  setStateBit(BUSY_BIT, 0);
  ms->stepPending = false;
  ms->stepped     = false;
}

void resetMotor() {
  stopStepping();
  if(GIE) {
    GIE=0;
    ms->curPos = 0;
    GIE=1;
  } else 
    ms->curPos = 0;
  setStateBit(MOTOR_ON_BIT, 0);
  for(uint8 i=0; i<4; i++)
    setMotorPin(motorIdx, i, 0);
}
void setStep() {
  if(ms->speed == 0) ms->speed = 1;
  uint16 stepTicks = CLK_RATE / ms->speed;
  if(stepTicks == 0) stepTicks = 1;
  uint16 nextStep = getLastStep() + stepTicks;
  setNextStep(nextStep);
  ms->stepped = false;
  ms->stepPending = true;
}

void chkStopping() {
  // in the process of stepping
  if(ms->stepPending || ms->stepped) return;
  // check ms->speed/acceleration
  if((ms->speed > sv->jerkSpeed)) {
    // decellerate
    uint16 accel = (ms->accelleration / sv->speed);
    if(accel > ms->speed) accel = ms->speed;
    ms->speed -= accel;
    setStep();
  }
  else {
    stopStepping();
    if(ms->resetAfterSoftStop) resetMotor();
  }
}

// from main loop
void chkMotor() {
  if(ms->stepped) {
    if(GIE) {
      GIE=0;
      if(ms->dir) ms->curPos++; else ms->curPos--;
      GIE=1;
    } else 
      if(ms->dir) ms->curPos++; else ms->curPos--;
    ms->stepped = false;
  }
  if(!haveError()) {
    if(ms->stopping)                  chkStopping();
    else if(ms->stateByte & BUSY_BIT) chkMoving();
  }
}

void softStopCommand(bool resetAfter) {
  ms->stopping = true;
  ms->resetAfterSoftStop = resetAfter;
}

void setMotorPins(uint8 motr, uint8 phase) {
  switch (phase) {
    case 0:
      setMotorPin(motr, 0, 1); setMotorPin(motr, 1, 1);
      setMotorPin(motr, 2, 0); setMotorPin(motr, 3, 0);
      break;
    case 1:
      setMotorPin(motr, 0, 0); setMotorPin(motr, 1, 1);
      setMotorPin(motr, 2, 1); setMotorPin(motr, 3, 0);
      break;
    case 2:
      setMotorPin(motr, 0, 0); setMotorPin(motr, 1, 0);
      setMotorPin(motr, 2, 1); setMotorPin(motr, 3, 1);
      break;
    case 3:
      setMotorPin(motr, 0, 1); setMotorPin(motr, 1, 0);
      setMotorPin(motr, 2, 0); setMotorPin(motr, 3, 1);
      break;    
  }
}

void motorOnCmd() {
  setStateBit(MOTOR_ON_BIT, 1);
  setMotorPins(motorIdx, ms->phase);
}

// no real homing in this mcu
void homeCommand() {
  if(GIE) {
    GIE=0;
    ms->curPos = sv->homePos;
    GIE=1;
  } else 
    ms->curPos = sv->homePos;
  setStateBit(HOMED_BIT, 1);
  motorOnCmd();
}

uint8 numBytesRecvd;

// called on every command except settings
bool lenIs(uint8 expected, bool chkSettings) {
  if(chkSettings && !ms->haveSettings) {
    setError(NO_SETTINGS);
    return false;
  }
  if (expected != numBytesRecvd) {
    setError(CMD_DATA_ERROR);
    return false;
  }
  return true;
}

//  accel is 0..7: none, 4000, 8000, 20000, 40000, 80000, 200000, 400000 steps/sec/sec
//  for 1/40 mm steps: none, 100, 200, 500, 1000, 2000, 5000, 10000 mm/sec/sec
const uint16 accelTable[8] = // (steps/sec/sec accel) / 8
       {0, 500, 1000, 2500, 5000, 10000, 25000, 50000};

void processCommand() {
  volatile uint8 *rb = ((volatile uint8 *) i2cRecvBytes[motorIdx]);
  numBytesRecvd   = rb[0];
  uint8 firstByte = rb[1];
  if ((firstByte & 0x80) == 0x80) {
    if (lenIs(2, true)) {
      // move command
      ms->targetSpeed = sv->speed;
      ms->accelleration = accelTable[sv->accelIdx];
      moveCommand(((int16) (firstByte & 0x7f) << 8) | rb[2]);
    }
  } else if ((firstByte & 0xc0) == 0x40) {
    // speed-move command
    if (lenIs(3, true)) {
      // changes settings for speed
      ms->targetSpeed = (uint16) (firstByte & 0x3f) << 8;
      ms->accelleration = accelTable[sv->accelIdx];
      moveCommand((int16) (((uint16) rb[2] << 8) | rb[3]));
    }
  } else if ((firstByte & 0xf8) == 0x08) {
    // accel-speed-move command
    if (lenIs(5, true)) {
      // changes settings for acceleration and speed
      ms->targetSpeed =(((uint16) rb[2] << 8) | rb[3]);
      ms->accelleration = accelTable[firstByte & 0x07];
      moveCommand((int16) (((uint16) rb[4] << 8) | rb[5]));
    }
  } else if ((firstByte & 0xe0) == 0x20) {
    // jog command relative - no bounds checking and doesn't need to be homed
    if (lenIs(2, true)) {
      motorOnCmd();
      uint16 dist = ((( (uint16) firstByte & 0x0f) << 8) | rb[2]);
      // direction bit is in d4
      if(firstByte & 0x10) ms->targetPos = ms->curPos + dist;
      else                 ms->targetPos = ms->curPos - dist;
      ms->accelleration = 0;
      ms->targetSpeed   = sv->jerkSpeed;
      moveCommand(true);
    }
  } else if (firstByte == 0x02) {
    // jog command relative - no bounds checking and doesn't need to be homed
    if (lenIs(3, true)) {
      motorOnCmd(); 
      ms->accelleration = 0;
      ms->targetSpeed  = sv->jerkSpeed;
      moveCommand(ms->curPos + (int16) (((uint16) rb[2] << 8) | rb[3]));
    }
  } else if (firstByte == 0x03) {
    // jog command relative - no bounds checking and doesn't need to be homed
    if (lenIs(3, true)) {
      motorOnCmd();
      ms->accelleration = 0;
      ms->targetSpeed  = sv->jerkSpeed;
      moveCommand((int16) (ms->curPos - (int16) (((uint16) rb[2] << 8) | rb[3])));
    }
  } else if (firstByte == 0x01) {
    // setPos command
    if (lenIs(3, false)) {
      if(GIE) {
        GIE=0;
        ms->curPos = (int16) (((uint16) rb[2] << 8) | rb[3]);
        GIE=1;
      } else 
        ms->curPos = (int16) (((uint16) rb[2] << 8) | rb[3]);
    }
  } else if (firstByte == 0x1f) {
    // load settings command
    uint8 numWords = (numBytesRecvd - 1) / 2;
    if (numWords > 0 && numWords <= NUM_SETTING_WORDS) {
      setMotorSettings(numWords);
    } else {
      setError(CMD_DATA_ERROR);
    }
  } else if((firstByte & 0xfc) == 0x04) {
   // next status contains special value
    if (lenIs(1, true)) {
      ms->nextStateSpecialVal = (firstByte & 0x03) + 1;
    } else {
      setError(CMD_DATA_ERROR);
    }
  } else if ((firstByte & 0xf0) == 0x10) {

    uint8 bottomNib = firstByte & 0x0f;
    // one-byte commands
    if (lenIs(1, (bottomNib != 4 && bottomNib != 7))) {
      switch (bottomNib) {
        case 0: homeCommand();               break; // fake home cmd
        case 2: softStopCommand(false);      break; // stop,no reset
        case 3: softStopCommand(true);       break; // stop with reset
        case 4: resetMotor();                break; // hard stop (immediate reset)
        case 5: motorOnCmd();                break; // reset off
        case 6: homeCommand();               break; // fake home cmd
        default: setError(CMD_DATA_ERROR);
      }
    }
  } 
  else setError(CMD_DATA_ERROR);
}

uint16 getLastStep(void) {
  uint16 temp;
  if(GIE) {
    GIE=0;
    temp = ms->lastStepTicks;
    GIE=1;
  } else 
    temp = ms->lastStepTicks;
  return temp;
}

void setNextStep(uint16 ticks) {
  if(GIE) {
    GIE=0;
    ms->nextStepTicks = ticks;
    GIE=1;
  } else 
    ms->nextStepTicks = ticks;
}

void clockInterrupt(void) {
  timeTicks++;
  for(int motIdx = 0; motIdx < NUM_MOTORS; motIdx++) {
    struct motorState *s = &mState[motIdx];
    if(s->stepPending && s->nextStepTicks == timeTicks) {
      if(s->stepped) {
        // last motor step not handled yet
        setErrorInt(motIdx, STEP_NOT_DONE_ERROR);
        return;
      }
      s->phase += (s->dir ? 1 : -1);
      if(s->phase ==   4) s->phase = 0;
      if(s->phase == 255) s->phase = 3;
      setMotorPins(motIdx, s->phase);
      s->stepPending = false;
      s->lastStepTicks = timeTicks;
      s->stepped = true;
    }
  }
}
