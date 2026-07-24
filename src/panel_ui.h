#ifndef PANEL_UI_H
#define PANEL_UI_H

#include "state_store.h"

void panel_ui_init(void);
void panel_ui_post_state(const display_state_t *state);

#endif /* PANEL_UI_H */
