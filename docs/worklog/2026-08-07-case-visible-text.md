# 2026-08-07 — Case-visible text, solved with colour instead of glyphs (`WC-2`)

Goal: you could not see what you typed in the WiFi password field. `glyph_rows()` folded `a-z` to `A-Z`, so every
mixed-case string in the UI rendered uppercase — a mistyped password was indistinguishable from a wrong one.

## TL;DR

- **The obvious fix does not work.** Lowercase glyphs in the existing 3×5 cell are impossible to disambiguate:
  with no descender row, `a`, `g` and `q` are *the same three rows*. "Tukang" rendered as "Tukana".
- **Growing the cell to 3×6** gives true descenders but makes every line 20% taller — a re-layout of every screen
  on both boards, for five letters.
- **The owner's idea beat both:** keep the single set of cap-shaped letterforms as the default and carry case in
  **colour** — a capital takes a fixed highlight (cyan; §5 covers why not the accent). No new glyphs, no
  ambiguity, no cell change, no re-layout.
- **Putting it on the panel immediately found the flaw:** every UI label was an uppercase literal, so the entire
  interface went accent-blue. Fixed by lowercasing **108 display literals**; they render as the same cap shapes,
  unmarked.

## 1. Why glyphs were the wrong answer

The font (`assets/pico8_font.h`) is a hand-authored 3×5 set, `'#'` = pixel on, shared with
`components/fake08/fps_hud.cpp`. It carries no third-party data and is not fake-08's, so editing it raised no
porting concern — the constraint was purely geometric.

3 wide × 5 tall on a shared baseline leaves **no row below the baseline**. Lowercase needs x-height forms plus
ascenders (fine in rows 0–4) *and* descenders for `g j p q y` (nowhere to go). Rendering candidate sets offline
with the device's own rasteriser made the failure obvious rather than theoretical:

| option | result |
|---|---|
| 3×5, no descenders | `a` ≡ `g` ≡ `q`. "Tukang Ketoprak" reads "Tukana Ketoprak". **Fails the one job.** |
| 3×6, true descenders | Works — at 20% taller text, so every vertical constant on every screen moves. |

Worth stating plainly: the first option would have *looked* like progress while leaving the password field just
as unverifiable. Drawing it is what caught that.

## 2. What shipped

Case is carried by colour. `case_col(c, fg, bg)` returns the highlight for `A-Z` and `fg` otherwise, and stands
down entirely on any non-default background: where a caller inverts a row, fg/bg are a contrasting pair and a
third fixed colour dropped into it reads worse than either. Wired into `draw_text` and the transparent `glyphs_into` used
for folder tiles.

`glyph_rows()` still folds for *lookup* — there is one glyph per letter — but case now survives as colour.

## 3. The flaw only the panel showed

First build on hardware: **the whole UI turned blue.** Labels like `"STATUS"`, `"SSID"`, `"FORGET NETWORK"` are
uppercase string literals, so every one of them was "all capitals" and took the accent. It also destroyed the
accent's existing meaning — selected row.

Fixed by rewriting **108 display literals to lowercase** (`"STATUS"` → `"status"`), which render as the identical
cap shapes with no marking. Deliberately untouched: `KB_LOW`/`KB_UPP`, which are the *characters the keyboard
types*, not labels. Nice side effect — shifting into `KB_UPP` now turns the keys accent, which reads as a caps
indicator for free.

Verified no `ESP_LOG` string was caught by the sweep.

## 4. Result

`Tukang Ketoprak` renders with `T` and `K` in the accent and the rest white; labels are white; the accent still
means "selected". Both boards build.

## 5. Follow-up: the capitals colour

Shipped first using the **accent**, which was wrong for two reasons that only became obvious side by side: the
accent already means *selected row*, and it is **user-configurable** — so capitals changed colour with the theme,
making case marking a theme decision rather than a property of the text.

Rendered five candidates at actual UI size (accent / caps-bright-rest-dimmed / amber / cyan / caps-bright-rest-
slightly-dim) and the owner picked **cyan**, now a fixed `s_case` independent of the accent. On the panel it
separates cleanly from the blue accent — better than the offline render suggested, which is the usual reason to
check on glass. `case_col()` still falls back to `fg` wherever the colour would be invisible or redundant.

## 6. Board state

P4 on the `FB_DUMP`/serial preview build during capture — **reflash the shipped touch build before calling it
known-good.** S3 built, not flashed with this change yet.

## 7. Review pass — eight findings, one of them a regression I introduced

**The highlighted-row protection had silently stopped working.** `case_col()` guarded with
`(s_case == bg || s_case == fg)`, which was correct only while `s_case` *was* the accent — an equality that
happened to catch inverted rows. Making it a fixed cyan broke that by construction: a fixed colour can never
equal the accent, so a capital on a selected pill rendered **cyan on accent**, the lowest-contrast text on the
screen (worst with the green theme). Both the code comment and the `WC-2` entry still described protection the
code no longer had. Now the highlight stands down whenever `bg != s_bg` — on an inverted row the caller has
already chosen a contrasting pair, and dropping a third fixed colour into it is never an improvement.

**Two literals escaped the sweep**, and one of them was the most-visited heading in the launcher:
`"SD CARD"` (the library breadcrumb, built as a `std::string` ternary rather than passed to a draw call) and
`"OPEN"` (the network-list sub-label, which on a highlighted row also bypassed the deliberate `fg = s_bg`
contrast choice). The sweep matched *call sites*, so anything assembled before the call was invisible to it —
worth remembering if this is ever repeated.

**`snprintf`-built strings were never literals at all**, so they were never in scope: `"%uMB FREE"`, `"%uK"` on
the download and confirm screens, and the board name. Units rendered cyan next to white digits, on the one
screen where the user is watching a number change.

**`"%dX%d"` coloured a multiplication sign** — `480X800` with the `X` marked as though it were a capital letter.
Actively misleading rather than merely noisy.

**Folder-tile capitals ignored the dim factor.** `draw_folder_tile` dims its text to ~47% for the side peeks to
say "not selected", but the highlight substituted full-brightness cyan regardless, punching straight through the
cue. `glyphs_into()` now takes the highlight colour as a parameter, dimmed by the caller alongside the body text.

Fixed all eight, plus the stale prose here and in the area doc. Nothing was wrong with the control flow — the
insertion points, the `val_max` clamp, and leaving `KB_LOW`/`KB_UPP` alone all checked out.

**Worth keeping:** two of these (the missed literals, the `snprintf` strings) are the same mistake in different
clothes — I verified the *mechanism* on the panel and assumed the *sweep* was complete because the screens I
happened to open looked right. The screens I did not open were the ones still broken.
