#ifndef _STATS_H
#define _STATS_H

#include <stdint.h>
#include <sys/time.h>

class EncodeStats {
    public:
        EncodeStats(float video_fps);
        void autoprint(uint32_t n_frames);
        void no_autoprint(void);
        void print(void);
        void reset(void);
        void print_and_reset(void);
        void print_cumulative(void);

        void input_bytes(uint32_t n_bytes);
        void output_bytes(uint32_t n_bytes);
        void finish_frames(uint32_t n_frames);
    protected:
        struct stats {
            struct timeval start_time;
            uint32_t frames;
            uint32_t bytes_in;
            uint32_t bytes_out;
        } cumulative_stats, current_stats;
        uint32_t autoprint_frames;

        void _print(struct stats *stat);

        float video_fps;
};

#endif
