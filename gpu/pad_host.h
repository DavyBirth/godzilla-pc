// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
union SDL_Event;
struct SDL_Window;
void gz_pad_event(const SDL_Event &event);
void gz_pad_poll();
void gz_pad_mouse_update(SDL_Window *window);
void gz_pad_self_test();
