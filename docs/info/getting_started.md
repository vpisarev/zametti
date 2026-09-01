# Getting Started

Zametti is a note taking program that uses Markdown (.md) format. For images it primarily uses JpegXL (.jxl) format.
The notes are organized in 'flat' storages, see the [Zametti storage organization](zametti-storage.md) for details.

Briefly, Zametti does not only keep all the notes, it also keeps the editing history of the notes, so you can get back to the past revisions and possibly find some later-removed fragments there.

Notes on the local disk are stored as plain markdown, so they can be edited or grepped with your favorite tools; notes in the cloud (currently, you can use any WebDAV server) are stored in a compressed and encrypted form, nobody could read them as long as they don't know 2 passwords: your WebDAV login password and the storage encryption password - you can have multiple storages and for each of them you can have an individual encryption password.

## Disclaimer

The program is distributed under GPL license and uses just open standards, open formats and open-source components.
This implies that the program will always stay open-source and you can always access your notes from virtually any computer, so don't worry about being locked-in or separated from your content.

On the other hand, use the software at your own risk (or don't use it at all). We don't provide any guarantees and will not be responsible for any damage that the software can make. Do periodical backups, it's always a good practice.

## Canonical markdown

When a markdown note is imported into the storage or is created by the user from scratch, it's stored in the storage in so-called canonical form:

- the note is given a 14-symbol [base-32](https://www.crockford.com/base32.html) name that consists of 8-symbol prefix: the unix time (elapsed seconds since 'Unix epoch', 01.01.1970 UTC) when the note is create & date and 6-symbol random suffix. The real name becomes the title (e.g. `# some verbose name of the note`)  This 14-symbol id is like a social id number, it keeps the note identity even if the displayed name changes. And the name can then contain any symbols, even emoji. See [zametti-storage](zametti-storage.md) for details.
- each markdown file starts with a html comment (which are supported by markdown parsers) with meta-information about the note. _Don't touch it if you edit the note with an external editor!_
- trailing whitespaces are erased. New-line symbol `\n` is added in the end.
- at the same time, user-added spaces in the beginning of each line, as well as extra empty lines between paragraphs are all preserved! So you can type unusually-formatted poems, for example, the formatting will be preserved.
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
  - `(url)` may include the current picture size in pixels, which changes as user adjusts the picture size by dragging one of the corners. Note that it only affects the displayed image size, not the image file in the storage.
  - it also may include optional alignment, which is center alignment by default.
  - if alt-text starts with `~` or if its name is too dull, e.g. `image 5` or `IMG_...` or `DSC...` or `Screenshot ...`, this text is not displayed under the picture. To display it, press `Ctrl+D` and edit it - make it more descriptive.
- all the referenced images (many different formats are supported by the application) are put into the storage (also under 14-symbol base32 names) in the (re)compressed form. Most of the time they become `.jxl` (JpegXL) files, but sometimes `.jpeg's` and `.webp's` are preserved if they cannot be recompressed into JpegXL accurately enough and/or with substantial savings in occupied space. Big images are downscaled with high-quality algorithm. There is user parameter `maxImportedImageSize` (see **Settings** below) that controls the downscaling - by default big images are downscaled to ~4.5MPix resolution, something in the middle between FullHD (2MPix) and 4K (8MPix). Note that JpegXL files can:
  - store up to 16 bits-per-channel images
  - support various color spaces (including sRGB, Display P3, AdobeRGB),
  - they preserve transparency information
  - they can even store lossless-transcoded jpeg images, so if you want to put a small jpeg into the note, don't worry, there will be no extra lossy recompression step.

The markdown notes are also transformed to a canonical form each time they are read from disk and each time they are written to disk. That is, the two invariants are preserved and verified by the multiple tests:

```
// x ~ .md file
serialize(parse(x)) == x   // byte-for-byte, for x in the
                           // canonical form in the storage
parse(serialize(parse(x))) == parse(x)  // for arbitrary x
```

Everything the program does not support (yet) — HTML, footnotes, quotes more complex than a paragraph — is preserved verbatim. Losing bytes is impossible, in theory at least, and it's verified by a bunch of tests.

## Preserving history

Note for mac users: read `Cmd` when you see `Ctrl`.
- Each note comes with its 'journal' of edits. The latest changes, as you type, become a part of in-memory undo/redo stack (`Ctrl+Z/Ctrl+Shift+Z`). The size of this undo/redo stack is only limited by memory. You can edit several notes at once, undo/redo stack for each of the notes is preserved within one editing session (i.e. until you quit the program)
- No need to press `Ctrl+S`, all notes are automatically saved
- All your modified notes are periodically auto-saved to disk, to `history/note_id.log` on-disk journals, where `note_id` is the same as the respective note `id`. The journals are well-compressed using `zstd`, see [storage description](zametti-storage.md) for details, so don't worry about the space.
- You can navigate through the note history by pressing `Note history` button on the toolbar or if you press `Ctrl+Z` when you reached the bottom of undo/redo stack. From the history you can choose whatever snapshot you like and restore it (think of it as of a super-simple alternative to git) or you can grab a piece of text from it and place into the current note.
- If some process changes your note on disk (or if you edit your note in the external editor), the program detects it and replaces the current note with the fresh content from disk, but this replacement becomes yet another editing operation, which you can undo.
- If you setup a cloud storage synchronization (webdav is supported, S3 will likely be supported in the future), your notes (the journals, actually) will be encrypted and stored there, so you can access them from another computer or restore them if your local copies are erased or damanged.
  - locally we always store all the content unencrypted for more convenience, safety and compatibility with external tools (vscode, grep, vim, ...).
  - we always encrypt all the notes and all the images when we store them in cloud.

## Settings

There are several types of user settings, stored in different places:
- `~/.config/zametti/config.json` - this is the file where you can configure how zametti looks and partially how it works. For the program it's a read-only file, only you edit it. To edit it, press `Settings` button on the toolbar or edit that file directly. It's inconvenient to start with an empty file, so you can run `zametti --dump-config` to get the initial fully-commented-off config, in which you can then uncomment and edit the sections and items that you want to alter. When you press `Settings` button and there is no config, the initial config with commented-off settings is automatically created, and then you edit it. Every key there carries a one-line explanation of what it does. The sections are:
  - `theme` — screen colors, see below;
  - `fonts` — the note font, the monospace font, the shell font, heading steps and the size of formulas. `appSize` is the one number that drives the whole shell: the tree, the note list, the status bar, the dialogs, and even the size of the toolbar icons are derived from it;
  - `layout` — column width, line height, spacing between blocks, list and quote indents;
  - `editor`, `shortcuts` — autosave, tab width, the external editor, and the key for every command;
  - `images`, `store`, `sync`, `pdf`, `logs` — the rest, each in its own place.

  What is *not* there is deliberate: optical constants of the renderer (how a bullet sits on the baseline, the corner radius of a code plate) live in the code, and the colors live in the theme.
- `~/.config/zametti/state.json` - this is the inter-session state that you normally want to preserve:
  - application window geometry. Some window managers don't let us to store the absolute position, they prefer to place windows as they wish. But the size is stored and then restored.
  - four independent zoom levels: the rendered note, the markdown source mode, the history/diff view and the shell itself. `Ctrl+=`, `Ctrl+-`, `Ctrl+0` change the one you are looking at; `Ctrl+Alt+=`, `Ctrl+Alt+-`, `Ctrl+Alt+0` change the shell (toolbar, tree, note list, status bar, dialogs). One step is about 6%, twelve steps are exactly twice as large. The levels never multiply each other, and none of them affects PDF export. They live here rather than in the config because the comfortable size differs from machine to machine — the same laptop under a different OS, or with an external monitor attached, wants a different one.
  - name of the recently viewed notes and cursor positions there.
  - etc.
- password for your storage is not stored as-is, but its Argon2id()-transformed representation is stored in the keychain and one of the components is stored in the cloud for verification. If you loose it, don't worry, just set the new password and re-upload your storage from one of your computers to the cloud again. That is, your notes will be lost only if you forgot the password and you erased all your local copies of the storage.
- password for your cloud storage is normally stored in your system keychain service for automatic synchronization without having to enter password each time. If you forgot one, also don't panic, generate new password with your WebDav provider and update your keychain; no need to re-upload storage in this case.

### Themes

All the screen colors — the page, the caret, the selection, list bullets, the code plate,
the toolbar icons, the status bar, markdown and JSON highlighting — are described by a
*theme*: a set of named **roles** such as `background`, `foreground`, `accent`,
`panelBackground` or `danger`. A role names a meaning, not a place, so one role usually
paints several things at once — `panelBackground` covers both the toolbar and the status bar.

The built-in theme is `light`. To tweak it, override the roles right in the config:

```jsonc
"theme": {
    "extends": "light",
    "accent": "#cc8822",
    "selectionBackground": "#fae8a8"
}
```

To keep a whole look of your own, put it in `~/.config/zametti/themes/<name>.json` and
point the config at it with `"extends": "<name>"`. A theme file may itself extend another
one; roles it does not mention stay as they were, so a three-line theme is a valid theme.
An unknown theme name, a loop in `extends` and a misspelled role are all reported rather
than silently ignored. Run `zametti --dump-config` to see every role with its default color
and a word about what it paints.

PDF export has its own colors (section `pdf`) and is not affected by the screen theme:
a dark screen still prints on white paper.

## Running

```bash
zametti               # open what you were reading or editing last time
zametti --help        # help: switches, keys, file paths
zametti --dump-config # get the initial .json, which can be put to
                      # ~/.config/zametti/config.json and edited to taste
zametti --root <storage_dir>  # switch to another storage;
                      # you can have as many storages as you want
```

## Using

The program uses a popular 3-panel interface:
- the left panel displays a tree of folders, initially it's just the root, more folders can be added by pressing `New folder` button.
- the middle panel displays all notes that belong to the selected folder and its subfolders.
  - the notes can be sorted in alphabetic order, by modification time (edit order) or by creation time (chronological order), press the corresponding button to change the order. The program remembers sorting order of each folder (it's highlighted with magenta color) _if_ you set it explicitly. You can reset the sorting order of the folder to the default order. The default order is defined by the current sorting order of the root folder. If a folder has custom sorting order, it's highlighted with magenta color. If the sorting order is the default one (matches the root folder sorting mode), it's highlighted with blue. For example, in your storage you may create a folder 'Diary' (probably with subfolders) for your daily notes and use chronological order so the notes stay in the same order as you created them, no matter if you get back to your older notes and fix some typos or add retrospective comments. Then, if you select all notes, you will see all notes in the root-sorted order (usually the 'edit order': most recently edited go first). But when you select Diary, the notes in the middle panel will be sorted in the chronological order - convenient!
- the right panel usually displays the viewed/edited note, however it can also display the currently observed snapshot when navigating through the note history, or the program config when you edit it (`Settings` button)

The left and middle panel can be hidden and then shown again by pressing `Hide side panels/Show side panels` button.

You can also press `F11` (or `Ctrl+Cmd+F` on macOS) to hide most of the content and concentrate on the note.

A new empty note can be created with `New note` button (or `Ctrl+N`) or imported from a markdown file from disk.

Editing a note is mostly intuitive, you can do it in the default WYSIWYG mode or raw 'markdown mode', repeatedly press `[M]` (`edit source`) button on the toolbar to switch between the two.

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
| Ctrl+Alt+= | make the interface (toolbar, panels, dialogs) bigger |
| Ctrl+Alt+- | make the interface smaller |
| Ctrl+Alt+0 | reset the interface size |
| Ctrl+F | start search in the current note |
| Ctrl+H | replace one text with another in the current note |
| Ctrl+Shift+F | start search over all notes |
| F3/Ctrl+G | find next |
| F4 | find the next difference in the history diff view |
| F5/Ctrl+R | re-read the storage folder and update the note, optionally run cloud sync |
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

## Deleting notes and restoring them

The storage includes so-called **Archive** and **Lost-n-found** special folders. If you want to delete some note, you first 'archive' it. It's moved to archive and stays there in read-only mode for as long as you wish.

You can restore archived note and continue to edit it at any time. All the history and attachments/images are preserved.

Or you can later decide to remove it completely. To do so, you select the note in archive and choose 'delete permanently' in the context menu. After confirmation the note is _almost completely_ deleted from the archive - only its latest snapshot in the compressed form (so called 'tombstone') is kept  in the journal and all its images, unless referenced by other notes, are resized to small stamps (e.g. 100x100, that's a user-adjustable parameter). Normally, such an 'erased' note consumes just a few kilobytes of the storage space.

Those 'completely erased' notes that then be restored using command line `zametti store resurrect --root <storage_root> --id <note_id>`, of course, with a complete loss of history and with seriously degraded images (no UI is currently provided for this black magic).

Now, suppose that someone sent you his/her notes from their storage (maybe together with images) and you directly copied the notes into your storage. Once you relaunched your application or pressed `F5`/(`Ctrl+R`), the program will find those notes and will place them into 'lost-and-found' folder in your storage. You can find them there and move to another folder.

## Managing storages and setting up cloud Synchronization

Click the left-most button on the toolbar to open the storage manager.

Using this dialog you can:
- Create new or add existing local storages to the program - press '+'
- Remove storages from the list, press '-'
- Locate your storages if you moved them to another location, press '...' (browse) button
- Setup WebDAV synchronization parameters:
  - provide url, login, password and optionally a custom folder to store your notes.
    - press 'Check' to confirm that the credentials you entered are valid.
  - provide encryption password for your storage.
  - there can be different cases:
    - if both the local folder and WebDAV folders are empty, the program (after getting confirmation from you) will create a brand new storage that you will gradually fulfill with notes.
    - if there is a valid zametti storage in the cloud and you created a new empty folder on the local disk, the cloud storage content will be downloaded as soon as you press 'Open'.
    - if there is empty folder on WebDAV server, your storage will be encrypted and uploaded to the cloud after you press 'Open'.
    - if both the local folder and WebDAV folder are valid zametti storages, the program will verify that they represent the same storage (of the same or different revisions) and the fresh content will be synchronized in both locations.
    - otherwise (different storages or non-empty folders that do not contain zametti storages) you will get an error.
  - if you happen to forget your encryption password, but you have at least one local copy with your notes, don't worry, you can reset the password, then your local content will be re-uploaded to the server. Press 'reset cloud for that'. That button can also be used if you want to change a provider, i.e. use another server for the same storage. On all the other computers you will have to enter the same encryption password.

---

That's the basic info about using the program. Enjoy it! :)
