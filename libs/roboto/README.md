# Roboto font

Roboto pre-rendered at 10 and 24 pt (at 150 dpi) by epdiy's `fontconvert.py`, taken
from `other-repos/projets/inky-ical`. The generated `roboto<size>.h` headers
originally included epdiy's `epd_driver.h`; they now include
`roboto_types.h`, which reproduces just the font types. The epdiy renderer is
replaced by `roboto_draw()` in `roboto_font.c`, which needs no framebuffer.

Only the sizes the firmware uses are kept: 10 pt for the status line and 24 pt
for the error screen. The other sizes (8-60 pt) are in the source repo if needed.
