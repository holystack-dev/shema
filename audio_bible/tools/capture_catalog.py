"""Single host-side catalog for complete device reviews and website exports.

Keep names aligned with the diagnostic route table in app_ui/capture_ui.inc.
"""

SCENES = [
    "home", "books-old", "books-new", "chapters", "psalms", "player",
    "sleep", "favourites", "saved-bible", "saved-edit", "saved-remove", "saved-year",
    "saved-library", "recent", "year", "year-days", "year-intros",
    "library", "library-folder", "library-nested", "letters",
    "settings", "versions", "screen-timeout", "power-off", "history-clear",
    "splash", "preview-no-card", "preview-empty-favourites", "preview-empty-saved",
    "preview-empty-recent", "preview-empty-library", "preview-empty-settings",
    "preview-player-playing", "preview-player-paused", "preview-player-year", "preview-player-library",
]
ALL_SCROLLS = {"settings", "sleep", "versions", "screen-timeout", "power-off", "letters", "preview-empty-settings"}
