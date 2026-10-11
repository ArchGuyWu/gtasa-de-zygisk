#pragma once

#include <cstdint>

struct GtasaSymbols {
    void *is_playing;
    void *find_player_ped;
    void *transition_started;
    void *transition_finished;
    void *start_cutscene;
    void *delete_cutscene;
    void *kill_player;
    void *arrest_player;
    void *respawn_hospital;
    void *respawn_police;
    void *shutdown_restart;
};

void plugins_set_jvm(void *vm);
void plugins_capture(int module_dirfd, const char *app_data_dir);
void plugins_capture_libs(const char *lib_dir, const char *app_data_dir);
void plugins_stage();
void plugins_run(uintptr_t base, const GtasaSymbols *symbols);
