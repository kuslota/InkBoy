#pragma once
#include <stdbool.h>

bool game_session_start(void);
/* Wake the emulator worker after an asynchronous terminal screen event. */
void game_session_wake(void);

bool game_session_ready(void);
const char *game_session_name(void);

bool game_session_select(void);
unsigned game_session_play(void);

bool game_session_ensure_saved(void);
/* False while the background writer has an unfinished request, i.e. SD I/O may be in flight. */
bool game_session_save_idle(void);
void game_session_save_state(void);
bool game_session_load_checkpoint(void);
