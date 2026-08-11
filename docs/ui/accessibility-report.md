# Basecamp Accessibility Report

The QML surface uses native controls, keyboard-focusable tabs and buttons,
wrapped status text, clipped lists, and a bounded rejection dialog. Interactive
controls have visible labels or `Accessible.name`. Connection state combines
text with color, and no private value is placed in a tooltip, clipboard action,
or notification.

Static asset tests cover the entrypoint, offline/loading/empty markers,
review/task/approval lists, rejection-sink confirmation, accessible names, and
absence of key/seed strings. Live screen-reader, localization, high-DPI, mobile
viewport, and Basecamp replica integration checks remain manual release gates
until the pinned Basecamp host is available in CI.
