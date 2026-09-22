// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// FMV feasibility probe for the LBA2 Nintendo 64 port.
//
// Plays every video file staged in the ROM's filesystem (one per codec and
// quality setting, produced by fmv-probe.sh from the retail VIDEO.HQR) and
// reports, on the ISViewer channel, whether the console keeps up: frames
// presented vs frames the stream contains, and video seconds per wall-clock
// second. Frames the player drops to stay in sync with the audio are the
// signal — a clip that plays at 1.00x with no drops is one the hardware can
// afford.
//
// This is a measurement harness, not port code: it is built against libdragon
// *preview* (the branch with the video module), while the game is built
// against the pinned trunk commit.
#include <libdragon.h>
#include <string.h>

#define AUDIO_HZ      32000.0f  // videoconv64 default audio rate (VADPCM)
#define PROBE_SECONDS 25.0f     // per clip: long enough to cross scene cuts
#define REPORT_EVERY  45        // presented frames between progress lines

typedef struct {
    const char *name;
    int presented;              // osd callbacks == frames actually displayed
    int first_idx, last_idx;    // stream frame indices seen
    uint32_t t_start, t_end;
    float last_time;
} clip_stats_t;

static clip_stats_t st;

static void osd_cb(void *ctx, int frame_idx, float time_sec, fmv_control_t *ctrl)
{
    if (st.presented == 0) {
        video_info_t info = video_get_info(ctrl->video);
        st.first_idx = frame_idx;
        st.t_start = get_ticks_ms();
        debugf("[fmvprobe] %s: %dx%d @ %.2f fps\n", st.name,
               info.width, info.height, info.framerate);
    }
    st.presented++;
    st.last_idx = frame_idx;
    st.last_time = time_sec;
    st.t_end = get_ticks_ms();

    // Report as we go: the numbers must survive the emulator being closed
    // half-way through a clip.
    if (st.presented % REPORT_EVERY == 0) {
        int src = st.last_idx - st.first_idx + 1;
        float wall = (st.t_end - st.t_start) / 1000.0f;
        debugf("[fmvprobe] %s t=%.1fs pres=%d src=%d skip=%d (%.1f%%) "
               "wall=%.1fs rt=%.2fx scr=%.1ffps\n",
               st.name, time_sec, st.presented, src, src - st.presented,
               src ? 100.0f * (src - st.presented) / src : 0.0f, wall,
               wall > 0 ? time_sec / wall : 0.0f,
               wall > 0 ? st.presented / wall : 0.0f);
    }

    if (time_sec >= PROBE_SECONDS)
        ctrl->stop(ctrl);
}

static void run_clip(const char *fname)
{
    char path[8 + sizeof(((dir_t *)0)->d_name)];
    snprintf(path, sizeof(path), "rom:/%s", fname);

    FILE *f = fopen(path, "rb");
    if (!f) {
        debugf("[fmvprobe] %s: cannot open\n", fname);
        return;
    }
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f);
    fclose(f);

    memset(&st, 0, sizeof(st));
    st.name = fname;

    fmv_play(path, &(fmv_parms_t){ .osd_callback = osd_cb });

    int src = st.last_idx - st.first_idx + 1;
    float wall = (st.t_end - st.t_start) / 1000.0f;
    debugf("[fmvprobe] %s (%ld KiB): %d/%d frames presented (%d skipped, %.1f%%)\n",
           fname, bytes / 1024, st.presented, src, src - st.presented,
           src ? 100.0f * (src - st.presented) / src : 0.0f);
    debugf("[fmvprobe] %s: %.2f s of video in %.2f s wall -> %.2fx realtime, "
           "%.1f fps on screen\n",
           fname, st.last_time, wall,
           wall > 0 ? st.last_time / wall : 0.0f,
           wall > 0 ? st.presented / wall : 0.0f);
}

static bool is_video(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcmp(dot, ".h264") || !strcmp(dot, ".m1v"));
}

int main(void)
{
    joypad_init();
    debug_init_isviewer();
    debug_init_usblog();
    dfs_init(DFS_DEFAULT_LOCATION);
    rdpq_init();
    yuv_init();
    audio_init(AUDIO_HZ, 4);
    mixer_init(8);

    video_register_codec(&mpeg1_codec);
    video_register_codec(&h264_codec);

    debugf("[fmvprobe] start, %ld KiB RAM\n", (long)get_memory_size() / 1024);

    // Every clip staged by fmv-probe.sh, in filesystem order; each one's audio
    // track is picked up by fmv_play from the matching .wav64.
    dir_t ent;
    int nclips = 0;
    for (int ok = dir_findfirst("rom:/", &ent); ok == 0; ok = dir_findnext("rom:/", &ent)) {
        if (ent.d_type == DT_REG && is_video(ent.d_name)) {
            run_clip(ent.d_name);
            nclips++;
        }
    }
    if (nclips == 0)
        debugf("[fmvprobe] no clips in the filesystem - run fmv-probe.sh first\n");

    debugf("[fmvprobe] done\n");
    while (1) { }
}
