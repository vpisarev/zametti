# zametti

take notes, organize 'em, encrypt, sync via cloud

Notes on disk are plain markdown: they can be edited with any tools and kept in git. The application does not own the format — it only reads and writes it.

## License

The project is licensed under GPL-3.0, see [LICENSE](LICENSE).The vendored or linked 3rdparty packages are distributed under their
respective licenses, see `3rdparty/*` and **How to build** below.

## Disclaimer

The license and the fact that we use open formats and open libraries, open standards (markdown, json, jpegxl, zstd, webdav ...) implies that the program will always stay open-source and you can always access your notes from virtually any computer, so don't worry about being locked-in or separated from your content.

On the other hand, use the software at your own risk (or don't use it at all). We don't provide any guarantees and will not be responsible for any damage that the software can make. Do periodical backups, it's always a good practice.

## Canonical markdown

When a markdown note is imported into the storage or is created by the user from scratch, it's stored in the storage in so-called canonical form:

- the note is given a 14-symbol [base-32](https://www.crockford.com/base32.html) name that consists of 8-symbol prefix (encoded seconds since 'Unix epoch', 01.01.1970 UTC when the note is created) and 6-symbol random suffix. The real name becomes the title (e.g. `# some verbose name of the note`)  This 14-symbol id is like a social id number, it keeps the note identity even if the displayed name changes. And the name can then contain any symbols, even emoji. See [zametti-storage](docs/zametti-storage.md) for details.
- each markdown file starts with a html comment (which are supported by markdown parsers) with meta-information about the note. _Don't touch it if you edit the note with an external editor!_
- trailing whitespaces are erased. New-line symbol `\n` is added in the end.
- at the same time, user-added spaces in the beginning of each line, as well as extra empty lines between paragraphs are all preserved! So you can type unusually-formatted poems - the formatting will be preserved.
- each heading starts with one or more `#`. We don't use under-the-heading `-----`.
- `-` is used for unordered lists. In WYSIWYG mode bullets of 3 different forms are used for unordered lists of different level, but it's just visualization
- ordered lists use `1.`, `2.` etc. enumeration. You can put `1.` everywhere, but it will likely be restored to `1.`, `2.` etc. on the next auto-save.In WYSIWYG mode nested lists use `a., b., c. ...` and `1), 2), 3) ...`, but it's just visualization.
- italic text is surrounded by `_`, e.g. `_emphasized words_` will be displayed as _emphasized words_.
- bold text is surrounded by `**`: `**that was a bold statement!**` -\> **that was a bold statement!**.
- strikethrough text is surrounded by `~~`. `~~zametti is yet another entry-level note taking app~~` -\> ~~zametti is yet another entry-level note taking app~~
- inline code fragments use ordinary back quotes, `print('hello world')`. Code blocks use explicit ` ```...``` ` instead of indentation.
- `$...$` surround inline formulas: $\text{circle area}=\pi r^2$; `$$...$$` are used for display formulas. However, it's possible to use `$...$` for display formulas as well - just place inline formula alone, separated from the rest of the text by empty lines.
- we don't use a 'markdown-standard' 4-space indent for lists. Instead, we use 2-space indentation for unordered lists and 3-space indentation for ordered lists.
- syntax for images is extended. There is a base `![alt-text](url)` standard notation, into which we add some extra stuff:
  - `(url)` may include the current picture size in pixels, which changes as user changes the picture size by dragging on of the corners. Note that it only affects the displayed image size, not the image file in the storage.
  - it also may include optional alignment, which is center alignment by default.
  - if alt-text starts with `~` or if its name is too dull, e.g. `image 5` or `IMG_...` or `DSC...` or `Screenshot ...`, this text is not displayed under the picture. To display it, type `Ctrl+D` and edit it - make it more descriptive.
- all the referenced images (many different formats are supported by the application) are put into the storage (also under 14-symbol base32 names) in the (re)compressed form. Most of the time they become `.jxl` (JpegXL) files, but sometimes `.jpeg's` and `.webp's` are preserved if they cannot be recompressed into JpegXL accurately enough and/or with substantial savings in occupied space. Big images are downscaled with high-quality algorithm. There is user parameter `maxImportedImageSize` (see **Settings** below) that controls the downscaling - by default big images are downscaled to ~4.5MPix resolution, something in the middle between FullHD (2MPix) and 4K (8MPix). Note that JpegXL files can:
  - store up to 16 bits-per-channel images
  - support various color spaces (including sRGB, Display P3, AdobeRGB),
  - they preserve transparency information
  - they can even store lossless-transcoded jpeg images, so if you want to put a small jpeg into the note, don't worry, there will be no extra lossy recompression step.

The markdown notes are also transformed to a canonical form each time they are read from disk and each time they are written to disk. That is, the two variants are preserved and verified by the multiple tests:

```
// x ~ .md file
serialize(parse(x)) == x   // byte-for-byte, for x in the
                           // canonical form in the storage
parse(serialize(parse(x))) == parse(x)  // for arbitrary x
```

Everything the program does not support (yet) — HTML, footnotes, quotes more complex than a paragraph — is preserved verbatim. Losing bytes is impossible, in theory at least, and it's verified by a bunch of tests.

## Preserving history

Note for mac users: read `Cmd` when you see `Ctrl`.
- Each note comes with its editing 'journal'. The latest changes, as you type, become a part of in-memory undo/redo stack (`Ctrl+Z/Ctrl+Shift+Z`). The size of this undo/redo stack is only limited by memory. You can edit several notes at once, undo/redo stack for each of the notes is preserved within one editing session (i.e. until you quit the program)
- No need to press `Ctrl+S`, all notes are automatically saved.
- All your modified notes are periodically auto-saved to disk, to `history/note_id.log` on-disk journals, where `note_id` is the same as the respective note `id`. They are well-compressed, see [storage description](docs/zametti-storage.md) for details, so don't worry about the space.
- You can navigate through the note history by pressing `Note history` button on the toolbar or if you press `Ctrl+Z` when you reached the bottom of undo/redo stack. From the history you can choose whatever snapshot you like and restore it (think of it as of a super-simple alternative to git) or you can grab a piece of text from it and place into the current note.
- If some process changes your note on disk (or if you edit your note in the external editor yourself), the program detects it and replaces the current note with the fresh content from disk, but this replacement becomes yet another operation, which you can undo.
- If you setup a cloud storage synchronization (webdav is supported, S3 will likely be supported in the future), your notes (the journals, actually) will be encrypted and stored there, so you can access them from another computer or restore them if your local copies are erased or damanged.
  - locally we always store all the content unencrypted for more convenience, safety and compatibility with external tools (vscode, grep, vim, ...).
  - we always encrypt all the notes and all the images when we store them in cloud.

## Settings

There are several types of user settings, stored in different places:
- `~/.config/zametti/config.json` - this is the file where you can configure how zametti looks and partially how it works. For the program it's a read-only file, only you edit it. To edit it, press `Settings` button on the toolbar or edit that file directly. It's inconvenient to start with an empty file, so you can run `zametti --dump-config` to get the initial fully-commented-off config, in which you can then uncomment and edit the sections and items that you want to alter.
- `~/.config/zametti/state.json` - this is the inter-session state that you want to preserve:
  - windows geometry. Some window managers don't let us to store the absolute position, they prefer to place windows as they wish. But the size is stored and then restored.
  - zoom factor: press `Ctrl+=`, `Ctrl+-` to increase/decrease scale of the edited note view.
  - name of the recently viewed notes and cursor positions there.
  - etc.
- password for your storage is not stored as-is, but its Argon2id()-transformed representation is stored in the keychain and one of the components is stored in the cloud for verification. If you loose it, don't worry, just set the new password and re-upload your storage from one of your computers to the cloud again. That is, your notes will be lost only if you forgot the password and you erased all your local copies of the storage.
- password for your cloud storage is normally stored in your system keychain service for automatic synchronization without having to enter password each time. If you forgot one, also don't panic, generate new password with your WebDav provider and update your keychain; no need to re-upload storage in this case.

## Running

```bash
zametti                # open what you were reading or editing last time
zametti --help         # help: switches, keys, file paths
zametti --dump-config  # get the initial .json, which can be put to
                       # ~/.config/zametti/config.json and edited to taste
zametti --root <storage_dir>  # switch to another storage,
                       # you can have as many storages as you want
```

## Using

The program uses a popular 3-panel interface:
- the left panel displays a tree of folders, initially it's just the root, more folders can be added by pressing `New folder` button.
- the middle panel displays all notes that belong to the selected folder and its subfolders.
  - the notes can be sorted in alphabetic order, by modification time or by creation time, press the corresponding button to change the order. The program remembers sorting order of the selected folder (it's highlighted with magenta color). You can reset the sorting order to the default order (sorting order of root folder, which is normally 'most recently edited first' and is highlighted with a blue color).
- the right panel usually displays the viewed/edited note, however it can also display the currently observed snapshot when navigating through the note history, or the program config when you edit it (`Settings` button)

The left and middle panel and be hidden and then shown again by pressing `Hide side panels/Show side panels` button.

You can also press `F11` (or `Ctrl+Cmd+F` on macOS) to hide most of the content and concentrate on the note.

A new empty note can be created with `New note` button (or `Ctrl+N`) or imported from a markdown file from disk.

Editing a note is mostly intuitive, you can do it in the default WYSIWYG mode or raw 'markdown mode', press `[M]` (`edit source`) button on the toolbar.

For your convenience, there are some auto-replacements and actions in WYSIWYG mode:
- `#SPACE`, `##SPACE` etc. start the new header (where `SPACE` means one press of the SPACE key)
- `*SPACE`, `-SPACE` start a new unordered list
- `1.SPACE` starts an ordered list
- `-[SPACE` starts a new task list
- `ENTER` on a list item adds a new item of the same kind below
- `ENTER` or double-click on selected formula or table enters raw-editing mode just for this object, `ESC` exists this raw-editing mode.
- `ENTER` on the selected picture enters image description editing mode, `ESC` exists it.
- `TAB`, `shift-TAB` increases/decreases indentation level of the current list item or selection

Here is the list of keyboard shortcuts that are supported (mac users: read `Cmd+` when you see `Ctrl+`, except for the fullscreen shortcut):

| Key | Action |
|:---:|--------|
| Ctrl+Z | undo the last editing operation; enter the note history if no more available undo ops |
| Ctrl+Shift+Z | redo the last editing operation |
| F11/Ctrl+Cmd+F | enter/quit fullscreen |
| Ctrl+= | increase zoom of the view |
| Ctrl+- | decrease zoom of the view |
| Ctrl+0 | reset zoom of the view to 1x |
| Ctrl+F | start search in the current note |
| Ctrl+H | replace one text with another in the current note |
| Ctrl+Shift+F | start search over all notes |
| F3/Ctrl+G | find next |
| F4 | find the next difference in the history diff view |
| Ctrl+A | select all |
| Ctrl+C | copy |
| Ctrl+X | cut |
| Ctrl+V | paste |
| Ctrl+B | make the selected text bold or not |
| Ctrl+I | make the selected text italic or not |
| Ctrl+/ | make the selected text strikethrough or not |
| Ctrl+E | convert the selected text to inline code or backwards |
| Alt+'-' | Enter long dash |
| Ctrl+ENTER | start a new paragraph after the current object (picture, formula, table, code block) |
| Shift+ENTER | continue the current list item after the current object (picture etc.) |
| Ctrl+D | toggle task(s) (done/undone) or edit picture description |
| Ctrl+Up | move the current list element up |
| Ctrl+Down | move the current list element down |
| Ctrl+7 | Convert the current list to ordered |
| Ctrl+8 | Convert the current list to unordered |
| Ctrl+9 | Convert the current list to task list |
| Ctrl+Shift+E | Convert the selected text block to code block |
| Ctrl+Shift+0 | Convert the selected text to normal text |


## Project structure

```
3rdparty/          everything third-party:
                   md4c, blake3, zstd, dtl, zlib, libtiff, highway,
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

## How to build and install

The application supports and was tested on Linux (Ubuntu), Windows and macos. Support for Android is planned.

You need to have development environment with Qt6 and a few other libraries. Many of the dependencies the application brings with itself (see 3rdparty), but not everything.

- On Debian/Ubuntu use something like:

  ```bash
  sudo apt update
  sudo apt install build-essential git cmake qt6-base-dev \
       qt6-base-dev-tools qt6-svg-dev libheif-dev libsodium-dev
  ```

- On macos you will need Xcode and brew,
  install necessary packages (Qt6 etc.) using brew.

- On windows: TBD

After everything is installed, use

```
cmake -DWITH_HEIF=ON -S . -B build
cmake --build build -j16
cd build && ctest
```

To make the application appear in the menu and with an icon in the dock:

```
cmake --install build --prefix ~/.local
```

The shell takes the icon not from the window but from the `.desktop` file, so without installing, the dock shows a placeholder. If you edit the code and want the edits to take effect at once, replace the installed copy with a symlink to the build — otherwise `cmake --install` has to be repeated after every rebuild:

```
ln -sf "$PWD/build/app/zametti" ~/.local/bin/zametti
```

## Tests

All suites live in ONE executable and run as one process. This is not for convenience: a suite that corrupts memory or the current directory will now affect the next one — a reason to investigate, not to shrug.

```
ctest                                       everything at once
build/tests/zametti-tests                   the same, directly
build/tests/zametti-tests --gtest_filter='Journal.*'   one suite
build/tests/zametti-tests --gtest_list_tests           what exists at all
```

Corpora are not checked into the repository: they are taken from `.testdata/` next to the sources or from the directory named in `ZAMETTI_TESTDATA`. No
corpus — the suite loudly reports the skip and passes empty.

Benches and probes are a separate program: they measure, they do not verify.

```
build/tests/zametti-bench                   the list of benches
build/tests/zametti-bench zoom              what Ctrl+= does to a document
```

Enjoy the program! :)
