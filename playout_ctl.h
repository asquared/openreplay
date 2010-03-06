#ifndef _PLAYOUT_CTL_H
#define _PLAYOUT_CTL_H

#define PLAYOUT_CMD_START_FILES 0x00
#define PLAYOUT_CMD_CUT 0x01
#define PLAYOUT_CMD_CUT_REWIND 0x02
#define PLAYOUT_CMD_ADJUST_SPEED 0x03
#define PLAYOUT_CMD_PAUSE 0x04
#define PLAYOUT_CMD_RESUME 0x05
#define PLAYOUT_CMD_STEP 0x06

struct playout_command {
    int cmd;
    int source;
    float new_speed;

    char filenames[512];
};

#endif
