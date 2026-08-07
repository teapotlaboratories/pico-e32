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

Case is carried by colour. `case_col(c, fg, bg)` returns the highlight for `A-Z` and `fg` otherwise. It stands
down where the caller has already made a colour decision: a true inversion (`bg == s_accent`), a deliberately
dim context row (which gets the *dimmed* highlight), and an accent-emphasised value. It does apply on plain
panels — the password field and the keyboard keys — which §8 covers, because a first attempt got that wrong. Wired into `draw_text` and the transparent `glyphs_into` used
for folder tiles.

`glyph_rows()` still folds for *lookup* — there is one glyph per letter — but case now survives as colour.

## 3. The flaw only the panel showed

First build on hardware: **the whole UI turned blue.** Labels like `"STATUS"`, `"SSID"`, `"FORGET NETWORK"` are
uppercase string literals, so every one of them was "all capitals" and took the accent. It also destroyed the
accent's existing meaning — selected row.

Fixed by rewriting **108 display literals to lowercase** (`"STATUS"` → `"status"`), which render as the identical
cap shapes with no marking. Deliberately untouched: `KB_LOW`/`KB_UPP`, which are the *characters the keyboard
types*, not labels. Nice side effect — shifting into `KB_UPP` marks the keys, which reads as a caps indicator for free.

Verified no `ESP_LOG` string was caught by the sweep.

## 4. Result

`Tukang Ketoprak` renders with `T` and `K` highlighted and the rest white; labels are white; the accent still
means "selected". (§5 changed the highlight from the accent to a fixed cyan; §8 fixed where it applies.) Both boards build.

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

## 8. Second review pass — the fix broke the thing it was protecting

The `bg != s_bg` guard from §7 was too broad. It was meant to stand down on *inverted* rows (a selected pill,
where fg/bg are a contrasting pair), but it fired on **any** non-default background — and the password field
draws on `s_platform`, the keyboard keys on `s_titlebar`. Both are plain panels, not inversions.

Net effect: **the password field had no case marking at all** — the single screen this whole feature exists for —
and the "shifting turns the keys accent" caps indicator I put in the PR body and the docs did not exist either.
Two claims documented, neither true of the merged code.

Corrected to test the inversion specifically (`bg == s_accent`) rather than "not the default background". Also
from the same pass:

- **Dimmed rows now get a dimmed highlight** (`s_case_dim`). `case_col` guarded on `bg` but never `fg`, so a
  context row drawn in `s_dim` — the confirm screen's `current`/`built`, the About build date — rendered
  full-brightness capitals through a row that is dim on purpose. The About row's lone bright `A` in
  "Aug  7 2026" read as a rendering fault rather than case information.
- **The case colour had two definitions.** The folder-tile highlight restated its RGB as a literal, so retuning
  `s_case` would have left tiles on the old cyan with no compile error. One `CASE_R/G/B` now feeds both.
- **Known and left as-is:** on the *selected* row case marking still stands down, so the SSID you are about to
  join is the one whose case is hidden. Fixing that needs a colour that contrasts against the accent rather than
  the background, which is a design question rather than a bug fix — noted here rather than guessed at.

**The pattern, twice now:** each fix was verified on the screens I happened to open, and each broke a screen I
did not. The password field was never re-captured after §7's change; the review caught it by reading the code
paths instead of the pixels.

## 9. Third review pass — the symmetric case, and button legends

**`case_col` handled dim rows but not accent-emphasised ones.** §8 added a carve-out for `fg == s_dim`; the
mirror case was missed. Where a caller deliberately draws a *value* in the accent on the normal background —
the Settings row showing a connected SSID, the confirm screen's `new` version — capitals fell through to the
cyan, so the row came out two colours and the "accent means connected/new" cue broke. A version string like
`OTA-TEST-2` rendered almost entirely cyan, defeating the emphasis it was given. Now an accent foreground is
left alone, exactly as `s_dim` is.

**The lowercase sweep hit character data, not just labels.** `"O SELECT   X BACK"` became
`"o select   x back"` — but `O` and `X` are *button legends*: the touch deck draws literal `O` and `X` glyphs
(`boards/…/board.cpp`), so the hint said lowercase `o` while the button beside it said `O`. Worse, under this
change's own convention an unmarked letter asserts "this is lowercase", which is simply false here. Restored as
`O select   X back` — the legend keeps its case (and gets marked, correctly), the words stay lowercase. This is
the same label-vs-character-data line that was drawn correctly for `KB_LOW`/`KB_UPP` and missed here.

**Documentation drift, three places.** The block comment still opened with "a capital is drawn in the accent"
two lines before "Deliberately NOT the accent"; this worklog's §2-§4 still described the rule that §8 reverted;
and the `WC-2` one-liner in the master TODO — the line most likely to be read — still said "capitals take the
accent". All corrected, and this file now says at the top that §8 holds the shipped rule, because burying the
correction at the end is what the review (rightly) called out.

**Three passes, three regressions of the same shape:** each fix was checked against the screens it was about,
and broke or mis-stated one it wasn't. What caught all three was reading the call sites rather than looking at
pixels — the pixels looked fine every time.
