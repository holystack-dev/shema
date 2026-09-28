# Shared UI rendering

The private `virtual_list.inc` and `virtual_grid.inc` modules are compiled once
inside `app_ui.c`. They share its theme, touch guard and LVGL lock. Page builders
supply content and callbacks; the renderers own measurement, placement, scrolling,
widget reuse and pressed-state styling. They perform no playback or persistence.

## Lists

Call `list_view_begin(container)`, add models with `list_view_add(...)`, then
`list_view_finish(initial_row)` (`-1` starts at the top). Check allocation failure
before adding models. Each model copies title/subtitle text and carries an integer
identity plus its action callback. Callbacks read **object user_data**, never the
renderer-owned event user_data. An optional remove callback exposes a separate
44px button with a 56px touch target; the row itself has no play callback in edit
mode. Confirmation remains the page's responsibility.

Eight LVGL row widgets cover the viewport plus overscan. The bounded 154-row model
is allocated once in PSRAM (about 58 KB). Library uses 150-entry
paging and a letter jump; stored list limits are compile-time checked against model
capacity. All text is measured before positioning, including wrapped subtitles.
`LV_USE_LARGE_COORD` is required: LVGL's small coordinate representation reserves
bits for percentage/content dimensions and cannot encode long lists beyond 8191px.

## Grids

`grid_view_begin` selects the book or chapter geometry and callback. Populate its
bounded identity array, set the selected model if needed, and call
`grid_view_finish`. It uses 12 book tiles or 24 chapter tiles for up to 150 chapters.
The model ID is the book ID or encoded book/chapter playback ID.
Changing testament replaces the model and removes the previous scroll handler.

## Ownership and lifecycle

Only one list and one grid controller are active on this single-screen device.
`build_page` detaches them before navigation. Screens own their widgets; routine
navigation immediately replaces the previous screen and fades the new artwork for
100ms without disabling input. Stale scroll/delete events check container identity
and cannot modify the new view.
Do not retain row widget pointers across scrolling: retain model IDs instead.
All calls and callbacks run under the LVGL lock; browsing storage remains separate
from the audio task's independent playback table.

`present_screen` owns navigation timing. Startup alone uses a 360ms eased fade
for both Home and the top-layer controls. Routine navigation uses an opacity
animation rather than LVGL screen-load animation: the latter disables touch
reading while `prev_scr` exists. The display backdrop is opaque `COL_BG`, matching
the page ground, so root opacity never exposes LVGL's default white backdrop.
Bottom touch geometry and feedback live in `bottom_control`. Back activates on PRESSED, emits one click and waits for that
contact to be released before accepting another action. Home Play activates on a
clean release. This prevents a held Back touch from activating Play after Home
appears. The Back target is 128×64 with its top edge at y290.

## Accidental-touch protection

Discrete controls register their action callback on `LV_EVENT_ALL` and gate it
with `tap_is_clean`. This includes Home, player transport/favourite/sleep, the
minibar, Edit/Done, modal backgrounds/options, lists/grids and setting choices.
The guard allows 14px of drift, remembers maximum travel even if the
finger returns, consumes each release once, cancels lost/deleted contacts, and
rejects a recycled widget whose model identity changed under the finger.
`guard_switch` disables LVGL's automatic pre-handler toggle, then emits a normal
VALUE_CHANGED only after a clean tap. Sliders drag continuously.
Back remains immediate on touch-down; Back gestures and modal-dismiss gestures
consume the rest of the contact before exposing the destination controls.

Accepted taps emit the audio click before running the action, so deleting a
screen cannot lose feedback. There is no global CLICKED sound; rejected
drags are silent. Slider releases emit their own feedback once.

## Regression checks

A development `BIBLE_SCREENSHOTS=1` build exposes `check-shared-views`,
`check-library-rows`, `navigation-benchmark`, `check-bottom-targets`,
`check-back-response`, `check-navigation-frames`, `check-accidental-touch` and
`check-confirmations`. They check visible labels/IDs/hit routing, every BIY day,
all 150 Psalms chapters, both testaments, both paging directions and letter jumps,
Back edges and cancellation without modifying user data. See
`../../tools/SCREENSHOTS.md`. Release firmware excludes these routes.

`python3 audio_bible/tools/test_played_cache.py` runs real progress code with
sanitizers against a temporary host directory. It covers completion revision
invalidation, folder boundaries, resume updates and cleared-slot reuse.
