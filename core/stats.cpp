#include "stats.h"
#include <sys/time.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>

EncodeStats::EncodeStats(float video_fps) {
    autoprint_frames = 0;
    this->video_fps = video_fps;
    memset(&cumulative_stats, 0, sizeof(struct stats));
    memset(&current_stats, 0, sizeof(struct stats));
    gettimeofday(&cumulative_stats.start_time, 0);
    gettimeofday(&current_stats.start_time, 0);
}

void EncodeStats::autoprint(uint32_t n_frames) {
    autoprint_frames = n_frames;
}

void EncodeStats::no_autoprint(void) {
    autoprint_frames = 0;
}

void EncodeStats::print(void) {
    _print(&current_stats);
}

void EncodeStats::reset(void) {
    memset(&current_stats, 0, sizeof(current_stats));
    gettimeofday(&current_stats.start_time, 0);
}

void EncodeStats::print_and_reset(void) {
    print( );
    reset( );
}

void EncodeStats::print_cumulative(void) {
    _print(&cumulative_stats);
}

void EncodeStats::input_bytes(uint32_t n_bytes) {
    current_stats.bytes_in += n_bytes;
    cumulative_stats.bytes_in += n_bytes;
}

void EncodeStats::output_bytes(uint32_t n_bytes) {
    current_stats.bytes_out += n_bytes;
    cumulative_stats.bytes_out += n_bytes;
}

void EncodeStats::finish_frames(uint32_t n_frames) {
    current_stats.frames += n_frames;
    cumulative_stats.frames += n_frames;
    if (current_stats.frames > autoprint_frames
            && autoprint_frames > 0) {
        print_and_reset( );
    }
}

void EncodeStats::_print(struct stats *stat) {
    struct timeval tv;
    int64_t delta_t;
    int32_t delta_usec;
    float fps, in_kbps, out_kbps;
    
    gettimeofday(&tv, 0);

    /* may be less than zero here */
    delta_usec = tv.tv_usec - stat->start_time.tv_usec;
    /* this is always >= 0 */
    delta_t = (tv.tv_sec - stat->start_time.tv_sec) * 1000000 + delta_usec;
    assert(delta_t >= 0);

    fps = (float)stat->frames * 1000000.0f / (float)delta_t;

    /* KB = 1024 bytes but kbps = 1000 bits per second ?? */
    in_kbps = (float)stat->bytes_in * 8.0f / (float)stat->frames / 1000.0f * video_fps;
    out_kbps = (float)stat->bytes_out * 8.0f / (float)stat->frames / 1000.0f * video_fps;

    fprintf(stderr, "frames:%d fps:%.3f in:%.1f kbps out: %.1f kbps\n",
        cumulative_stats.frames, /* always use cumulative frame count */
        fps, in_kbps, out_kbps
    );
}
