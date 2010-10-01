#ifndef _PLAYOUT_CTL_H
#define _PLAYOUT_CTL_H

#define MAX_CHANNELS 64

/*
 * This file defines the format of the messages used to control
 * the playout daemons.
 */

#define PLAYOUT_CMD_CUE 0x00 
#define PLAYOUT_CMD_CUT 0x01
#define PLAYOUT_CMD_CUT_REWIND 0x02
#define PLAYOUT_CMD_ADJUST_SPEED 0x03
#define PLAYOUT_CMD_PAUSE 0x04
#define PLAYOUT_CMD_RESUME 0x05
#define PLAYOUT_CMD_STEP_FORWARD 0x06
#define PLAYOUT_CMD_STEP_BACKWARD 0x07

struct playout_command {
    int cmd;
    int source;
    float new_speed;

    int marks[MAX_CHANNELS];
};

struct playout_status {
    int timecode;
    int active_source;
    int valid;
};

#endif
