# zametti

take notes, organize 'em, encrypt, sync via cloud

Notes on disk are plain markdown: they can be edited with any tools and kept
in git. The application does not own the format — it only reads and writes it.

## Status

Stage 1 is closed: the format core and the viewer. Stage 2 — editing — is
underway.

```
3rdparty/          everything third-party: md4c, blake3, zstd, dtl, zlib, libtiff, highway,
                   libjxl, jpegli, libwebp, microtex, googletest
zametti-core/      the core as one target:
                     format/  markdown parsing and writing, hashes, ids
                     store/   the store, the edit journal, archive, times
                     image/   image reading and writing, color, import
                     doc/     the live document model, search, counting, diff
app/               zametti-ui: the window, the note tree, rendering
store/             zametti-store: the store utility
tests/             test suites (one process) and benches
packaging/         .desktop for the menu and the dock
docs/              stage briefs, reports and decision notes
```

The core does `markdown → IR → markdown`. Its main property is idempotence:

```
serialize(parse(x)) == x                byte-for-byte, for x in canonical form
parse(serialize(parse(x))) == parse(x)  for arbitrary x
```

Everything the model cannot express — tables, HTML, footnotes, quotes more
complex than a paragraph — is preserved verbatim. Losing bytes is impossible.

## Build

```
cmake -S . -B build
cmake --build build -j
cd build && ctest
```

The core needs `qt6-base-dev` (that is where `QTextDocument` lives), the
viewer also `qt6-svg-dev`. Everything third-party except Qt is linked into the
program — including the image readers: none of our formats depends on Qt
plugins.

Everything third-party except Qt is linked into the program. Two switches:

```
-DWITH_HEIF=ON        read avif and heic (needs the system libheif)
-DWITH_STATIC_QT=ON   refuse to build if the Qt found is dynamic
```

`WITH_HEIF` is off by default: libheif drags video codecs along, and we need
avif/heic only on input. `WITH_STATIC_QT` switches nothing on — Qt becomes
static not because of flags but because it was built that way; the switch
merely refuses to silently produce an ordinary build where a self-contained
one was asked for.

## Run

```
zametti note.md               window: tree on the left, document on the right
zametti                       open what you were reading last time
zametti --noconfig note.md    the same, but on defaults
zametti --check note.md       diff against the canonical form, no display
zametti --dump-config         the full list of appearance parameters
zametti --help                help: switches, keys, file paths
```

`Ctrl+=` / `Ctrl+-` / `Ctrl+0` change the zoom, `Ctrl+S` saves,
`Ctrl+Z` / `Ctrl+Shift+Z` undo and redo. Clicking a folder in the tree
expands it, clicking a note opens it.

`Enter` in ordinary text breaks the line **inside the paragraph** instead of
starting a new one — that is how notes are written, not the word-processor
way. A new paragraph comes from a second `Enter` in a row, i.e. a blank line,
just as in the file itself. `Shift+Enter` does the opposite.

That is how poems, lists of lines and character diagrams are written. Indents
at the start of lines are preserved: markdown eats an ordinary leading space,
so on write the indent becomes a non-breaking space. Trailing spaces, on the
contrary, are dropped — they are insignificant.

Blank lines are preserved too: they set chunks of text apart. The only things
not preserved are the tail of blank lines at the very end of a note and empty
items inside a list.

The indent preserved is the one typed here. From an already existing file it
cannot be recovered: parsing eats it there, and it has nowhere to come from.

A code block is started with three backticks (or tildes) and `Enter`; the
language goes right after them, on the same line. To leave the block — a
fence on the last line. A blank line does not leave the block: in long code
blank lines separate logical parts. Inside the block, spaces are ordinary,
with no tricks whatsoever: code gets copied from a note and pasted into a
terminal.

`Enter` at the start of an item creates an empty item above the current one,
and the cursor stays in it: that is how an item is inserted between two.
Press `Enter` there twice — and the item leaves the list, becoming a
paragraph: that is how two stuck-together lists are pulled apart. A blank
line will not do here — inside a list it separates nothing.

In lists: `Enter` starts a new item, and on an empty item leaves the list;
`Backspace` at the start of an item turns it into a paragraph; `Tab` and
`Shift+Tab` move **only this item** (or the selected ones) across levels —
nested items keep their levels and become its siblings, and on outdent they
are pulled in by one level so there is no jump across a level; `Ctrl+Up` and
`Ctrl+Down` move an item among its same-level neighbors together with its
nested items; `Ctrl+D` toggles a task — done or not, again only this one (or
the selected ones). All three shortcuts are configurable:
`editor.toggleTaskKey`, `editor.moveUpKey`, `editor.moveDownKey`.

The kind of blocks is changed by the context-menu commands — "Make bulleted
list" and the rest. A mixed selection is converted to one kind as a whole,
not toggled. The shortcuts follow the symbol on the key: `Ctrl+8` or
`Ctrl+Shift+8` — bulleted list, `Ctrl+7` — numbered, `Ctrl+9` — tasks,
`Ctrl+Shift+0` — plain text.

Heading levels have no shortcuts: a heading is typed by autoreplace — `# `,
hashes and a space. The levels remain in the context menu.

All shortcuts are configurable (`editor.makeBulletKey` and so on); several
can be listed separated by semicolons, and an empty string removes the
shortcut entirely — the command still stays in the menu.

Text styles: `Ctrl+B` — bold, `Ctrl+I` — italic, `Ctrl+/` — strikethrough,
`Ctrl+E` — inline code. On a selection they change it; with no selection they
set the style for the next letter.

Inline code can also just be typed: backtick, text, backtick — the backticks
go away, the text becomes code. `Ctrl+Shift+E` turns the selection into a
code block, and a code block back into plain text.

The easiest way to mark a task is the mouse: clicking the box toggles it, and
that is an ordinary edit — undone like any other.

The bullet changes its shape with nesting depth: a solid circle, a hollow
one, then a square. Configured by the `list.bulletShapes` list.

A code block sits on a plate with a strip along its bottom edge: the language
name and the copy button live in the strip's right corner. One knob sets its
size — `layout.codeLangPointSize`, the point size of that caption: the strip
height (`layout.codeStripHeight`, in caption line heights) and the copy icon
(`layout.codeCopyIconScale`) follow it, and so does the width of the inline
field that edits the language. The caption typeface is `font.codeLangFamily`.

Autoreplace works when typing at the start of a block: `- `, `* ` and `+ `
give a bullet, `1. ` and `1) ` — a numbered item, `# `…`###### ` — a heading.

A task list is started in two ways. The short way — flush with the marker:
`-[`, `-[]`, `-[x]` and a space; the closing bracket need not be typed. The
long way — as in the file: `- `, then `[ ] ` or `[x] `. The first `Ctrl+Z`
after an autoreplace brings back the typed characters rather than undoing the
previous edit.

## Editing the source

The `[M]` button in the button strip shows the note as **raw markdown** — the
way it lies in the file, with syntax highlighting (headings `#` and `##`
larger, `###` and deeper — at text size, in bold italic;
`markdownHighlighting.largeHeadingLevels`). There it is edited as plain
text, but lists are understood: `Enter` on an item line (`- `, `* `, `1. `,
`- [ ] `) starts the next item — with the same indent, the next number, an
unchecked task — and on an empty item leaves the list; on other lines `Enter`
keeps the indent of the previous one (handy both in code and in multi-line
items); `Shift+Enter` continues the item with a content line — the caret
lands under the first character after the marker. `Tab` on an item line
moves the item under its previous sibling (the first item does not move),
`Shift+Tab` — to the parent's indent; outside lists `Tab` inserts spaces up
to the nearest tab stop, `Shift+Tab` removes them; a multi-line selection
moves as a whole. `Ctrl+D` (`editor.toggleTaskKey`) toggles the task of the
line or the tasks of the selection. Markers are typed literally — there are
no autoreplaces in the source, they get highlighted as they are.
`Ctrl+C`/`Ctrl+V`/`Ctrl+X`/`Ctrl+A` and search with replace (`Ctrl+F`,
`Ctrl+H`, `F3`) — as everywhere. The mode has **its own undo stack**:
`Ctrl+Z` undoes text edits step by step, each of the keystrokes listed above
being one step; at the bottom of the mode's stack `Ctrl+Z` closes the mode
(there is nothing left to apply) and hands undo over to the note — its
stack, and the history after it, as in the normal view. The caret has the
same color and thickness as in the normal view. Lines wrapped to the window
width carry a dot in the left margin — so it is visible where a source line
continues and where a new one begins.

Leading spaces of paragraph lines **are preserved** — a poem with indents or
text drawn in ASCII art will not fall apart: markdown would eat them, so on
parsing they become non-breaking spaces (U+00A0) and are stored in the file
the same way. Structural indent (item nesting, the content column) does not
count. Trailing spaces are cleaned as before. One rule for everything:
opening a file, returning from the source and editing the note in an external
editor all take the same path.

`[M]` again returns the normal view — `Esc` deliberately does not: leaving the
mode applies everything typed here to the note, and that is not something a key
under your fingers should do by accident. The mode has no default
shortcut — `Ctrl+M` on a Mac is `Cmd+M`, "minimize window"; whoever needs a
key writes it into `editor.markdownModeKey`. The edited text is brought to
the canonical form and applied to the note **only in the touched pieces** —
in the note's undo stack it is one step, and `Ctrl+Z` brings everything back
at once. The caret stays on the same line in both directions.

The mode belongs to the **application**, not to the note: you can walk across
notes without leaving it — the button stays pressed, and every next note
opens as source. This state survives a restart.

The zoom of the plain-text views is **their own, and it is one**: `Ctrl+=` /
`Ctrl+-` / `Ctrl+0` in the source do not touch the zoom of the normal view and
vice versa, while the source and the settings editor always share the same
size — they are the same text in the same font, and opening one of them
smaller than the other would make no sense. The source is shown in the code
font (`font.codeFamily` at `font.pointSize` — no separate setting of its own),
the column is limited to the same width as in the normal view
(`style.maxContentWidth`), code blocks go on a light gray backing — like
inline code.

The `<!-- zametti … -->` header is not shown by the source view — the
application itself owns it; typed in by hand, it is rejected together with
the whole edit, which the status bar reports.

Undo works on content, not on appearance: zoom, font and colors do not enter
the edit history. Type some text, enlarge the font, press `Ctrl+Z` — the
previous text comes back, the font stays enlarged.

Only a changed note is saved: simply opening a file and looking at it is
safe, on disk it does not change. Before the file is replaced, the written
text is parsed back and checked against the document; if it does not match —
the file stays untouched, and the buffer goes to `name.md.rescue-<timestamp>`.

To make the application appear in the menu and with an icon in the dock:

```
cmake --install build --prefix ~/.local
```

The shell takes the icon not from the window but from the `.desktop` file, so
without installing, the dock shows a placeholder. If you edit the code and
want the edits to take effect at once, replace the installed copy with a
symlink to the build — otherwise `cmake --install` has to be repeated after
every rebuild:

```
ln -sf "$PWD/build/app/zametti" ~/.local/bin/zametti
```

## Settings

Appearance is read from `~/.config/zametti/config.json`. The file is yours,
formatting and key order included; it may not exist at all, then the defaults
apply. The config is a list of **deviations** from the defaults, not a copy of
them: remove a key and the default comes back.

The gear button in the toolbar opens that file **inside the application**, on
the place of the editor, with JSON highlighting — the same page mechanics as
the source mode: press the button again or `Esc` to leave (unlike the source
mode, `Esc` does work here — leaving only writes the file), `Ctrl+S` to write,
`Ctrl+F` and `F3` to search. `Tab` inserts spaces up to the next stop
(`jsonEditing.tabIndent`, four by default), `Enter` keeps the indent of the
previous line, `Ctrl+/` (`jsonEditing.commentKey`) comments and uncomments the
line or the selected lines. Undo and redo are the ordinary ones and live for
as long as the editing session. The caret is the same one as in a note — its
color and width come from the same settings.

The file is written on `Ctrl+S`, on leaving the mode and on exit — there are
no confirmation dialogs anywhere in this program. Text that does not parse is
still written (it is your file) but is not applied: the status bar says what
is wrong, and the previous values keep working. Colors of the highlighting are
the `jsonEditing` section.

Editing the file with an external editor works exactly as before: the
application watches it and reloads the appearance on the fly.

The full list of parameters with their default values is printed by
`--dump-config` — that is where to copy them from. Configurable are the
typeface and size (separately for text and for code), line spacing, block
spacing, margins and the maximum column width, all colors, list markers and
checkboxes, the note tree, the zoom limits. Spacings and margins are given in
font units — vertically in line heights, horizontally in widths of the
letter `A` — so they survive a change of typeface and size.

If a note was changed from outside while there are unsaved edits here, the
application asks whose version to take and touches nothing until answered.
Without unsaved edits the external content is applied by itself — and is
undone with `Ctrl+Z`, like an ordinary edit.

`~/.config/zametti/state.json` the application writes itself on exit: the
last note, scroll position, zoom, window geometry, expanded tree branches.

## Tests

All suites live in ONE executable and run as one process. This is not for
convenience: a suite that corrupts memory or the current directory will now
affect the next one — a reason to investigate, not to shrug.

```
ctest                                       everything at once
build/tests/zametti-tests                   the same, directly
build/tests/zametti-tests --gtest_filter='Journal.*'   one suite
build/tests/zametti-tests --gtest_list_tests           what exists at all
```

Corpora are not checked into the repository: they are taken from `.testdata/`
next to the sources or from the directory named in `ZAMETTI_TESTDATA`. No
corpus — the suite loudly reports the skip and passes empty.

Benches and probes are a separate program: they measure, they do not verify.

```
build/tests/zametti-bench                   the list of benches
build/tests/zametti-bench zoom              what Ctrl+= does to a document
```

Examples from the CommonMark and GFM specifications are parsed by the script
`tests/extract_spec.py`.

- [docs/zametti-m1-report.md](docs/zametti-m1-report.md) — stage summary:
  what was done, how it was verified, what broke along the way.
- [docs/zametti-core-notes.md](docs/zametti-core-notes.md) — accepted
  decisions, deviations from the brief, known limitations.
- [docs/zametti-editor-notes.md](docs/zametti-editor-notes.md) — stage 2: the
  document model, the way back into IR, layer separation.

## License

GPL-3.0, see [LICENSE](LICENSE). The vendored md4c is MIT, see
[3rdparty/md4c/LICENSE.md](3rdparty/md4c/LICENSE.md).
