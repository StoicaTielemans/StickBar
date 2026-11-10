#ifndef CUSTOMBAR_RENDER_POPUP_STYLE_H
#define CUSTOMBAR_RENDER_POPUP_STYLE_H

// Layout tokens every bar dropdown shares -- the header/pill/card rhythm all
// of them were restyled to (docs/popups.md "Look"). They used to be
// redeclared, with the same values, at the top of each popup file. A popup
// whose value really differs (the calendar's wider margin, the tray menu's
// tighter cards) keeps its own local #define, saying why.

#define POPUP_OUTER_MARGIN 12.0    // popup edge -> content
#define POPUP_SECTION_GAP 10.0     // header -> first card, between sections
#define POPUP_CARD_GAP 8.0         // between stacked cards
#define POPUP_COL_GAP 10.0         // between columns in a row
#define POPUP_HEADER_H 22.0
#define POPUP_HEADER_ICON_GAP 8.0  // header icon -> title
#define POPUP_PILL_H 16.0
#define POPUP_PILL_PAD_X 7.0
#define POPUP_PILL_GAP 16.0        // header text -> pill, when sizing a popup to its header

// The y of the first card: under the header and its gap.
#define POPUP_FIRST_CARD_Y (POPUP_OUTER_MARGIN + POPUP_HEADER_H + POPUP_SECTION_GAP)

#endif
