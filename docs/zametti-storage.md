# Store layout

This document describes what lies on disk: how the note store is organized,
what a note file consists of, what an attachment is and how the history is
recorded. It is a reference for the format, not for the code; everything said
here is checked by `zametti-store verify` and by the test suites.

The main property: **the store can be read and edited without the program**.
A note is a plain markdown file in UTF-8, the history is a file next to it.
Should the program vanish — the texts remain.

---

## 1. The directory

The store is flat. No nested folders: the hierarchy lives in the metadata,
not in the file system.

```
store/
    01n6cqevh7bbfr0a.md        a note
    01n6cqevsd7v5edf.md        a note
    01jd7f0kq2m8xab7.webp      an attachment
    history/
        01n6cqevh7bbfr3v.log   edit history of this note
    .zametti/
        store.lock           the lock: one store — one program
    .rescue/
        …                    buffers that failed the write self-check
```

Three reserved names and their fate under synchronization:

| name | what it is | synchronized |
|---|---|---|
| `history/` | edit history | **yes**: it is data, not a cache |
| `.zametti/` | store state | no |
| `.rescue/` | rescued buffers | no (dot-prefixed, local) |

Settings and window state live **not** in the store but in the application
config (`config.json` and `state.json` in the user's settings directory): the
store belongs to the notes, not to the program.

Anything else in the root is trouble, and `verify` will say so: a foreign
directory, a foreign file, a file with a malformed name.

---

## 2. The file name: the note id

The file name is the note's identifier, **14 characters of Crockford base32**
in lower case (`0-9`, `a-z` without `i l o u`), plus the extension:

```
01n6cqevh7bbfr.md      a note
01jd7f0kq2m8xa.webp    an attachment
```

Inside the id: 8 characters are the big-endian unix seconds of creation with
leading zeros, 6 are random from a CSPRNG. For the next thousand years every
id will start with `0` — a de-facto marker of the format.

Two rules that matter more than the internals:

- **the id is opaque.** The time is never parsed out of it; the real creation
  date lives in the metadata. The time prefix is there only so that `ls`
  sorts by age and the eye has something to hold on to while debugging;
- **the name means nothing to a human.** A note's title is its first line,
  not the file name. So renaming a note does not touch the file at all, and
  links between notes do not break when a title changes.

The file is created strictly under a fresh name (`O_EXCL`); a collision is
the rarest of cases, and then the id is regenerated.

---

## 3. The note file

```markdown
<!-- zametti
version: 1
parent: 01n6cqevr3wprw
created: 2017-06-19T07:54:29Z
modified: 2026-07-30T19:26:24Z
-->

# TODO before the trip

Put the needed info/programs onto the mac:
- [ ] wechat
- [ ] fresh versions of opencv
```

The header is an **ordinary HTML comment**, so any markdown viewer will
simply not show it. The first line of the comment is the word `zametti`, then
lines of the form `key: value`.

Known keys:

| key | meaning |
|---|---|
| `version` | note format version; no key — `1`. Set LAZILY: only on notes the program writes itself — fresh, imported and edited ones (together with the `modified` stamp); opening and viewing a file does not touch it. A newer version is never downgraded |
| `parent` | id of the parent note; no key — the note is at the root |
| `created` | when it was created, ISO-8601 with an offset |
| `modified` | when the content was edited, ISO-8601 with an offset |
| `role` | `folder` — a folder note, `lost` — the Lost & found |
| `archived` | `yes` — the note is put away into the Archive; no key — alive |
| `lost-parent` | the find's previous parent; set by the Lost & found |
| `sort` | sort order inside the folder; no key — inherited |

Rules for the header content:

- **unknown keys are never touched.** The read-write round trip is
  byte-exact, including line order and foreign keys: the program edits only
  its own line, replacing it with the canonical form;
- `modified` is bumped only on a **content** edit. Moving a note, putting it
  into the archive, bringing it back and registering it in the Lost & found
  are not editing, and the note must not float to the top of the recent list
  because of them;
- **times are ISO-8601 with an offset** (`2026-08-14T21:40:00+02:00`): one
  line carries both the absolute instant and the local context — what the
  clock showed for whoever was writing. Three forms are read: with an offset,
  the old UTC (`…Z`) and with no zone at all — the latter is interpreted in
  the zone of the READING machine (there is no other reasonable assumption).
  Migration is lazy: any save rewrites a zone-less or bare-UTC stamp into the
  new form, and does not touch a stamp with an offset — it records someone
  else's local context, and converting it into one's own zone would erase
  that. Comparisons and sorts go BY INSTANTS, not by strings;
- **a note never becomes a folder, and vice versa** (owner's rule).
  `role: folder` is set at creation and does not change. A folder's body is
  exactly one heading and nothing else — `verify` watches over this;
- `sort` is a label of the FOLDER: how its notes and subfolders are ordered.
  The value is `<key>-<direction>`, where the key is `name`, `modified` or
  `created`, and the direction is `asc` or `desc`; the direction may be
  omitted (`sort: created`), then the key's default is taken — names A→Z,
  dates newest first. The label is inherited downward: a folder without its
  own goes by the nearest labeled ancestor, and if none is labeled — by the
  toolbar switch (that one lives in `state.json`; it is not in the store and
  there is nothing to synchronize). An unrecognized value is not an error:
  the program complains to stderr and shows the folder by inheritance, while
  the key in the file stays untouched. Setting the label does NOT bump
  `modified`: it is an organizational edit, like a move.

The body is markdown in canonical form: what the program writes equals
`serialize(parse(file))`. Drift between the two is trouble, and `verify`
hunts for it. On open, a note is tidied right on disk (stray spaces at the
ends of lines, a missing newline at the end of the file) without touching a
single value in the header.

---

## 4. Attachments

An attachment is a file next to the notes, with the same 14-character id and
its own extension: `01jd7f0kq2m8xa.webp`. Attachments have no separate
directory — the store is flat all the way down.

A link in the text is an ordinary markdown image, the display size in the
fragment:

```markdown
![view from the balcony](01jd7f0kq2m8xa.webp#w=600&align=left)
```

- attributes live in the address fragment, joined by `&`: `w=600` — display
  width in logical pixels, `align=left|center|right` — alignment. Both are
  optional; by default the image goes centered, book-style;
- the wiki form is understood too: `![[01jd7f0kq2m8xa.webp|600|align=left]]`.
  On import it is rewritten into the canonical form above;
- identical content — one file: on import attachments are deduplicated by
  sha256.

**Whether to show the caption (alt) is decided by a rule, not by the file**
(17–17.08.2026, owner's decision: "show good names, hide silly ones"). In
the file the caption always lies as it is; under the picture it is NOT shown
when it is nameless — `isNonameCaption`:

- empty: `![](01jd….webp)` — a legitimate captionless image, must be
  preserved in any case;
- hidden by a person with one character in front: `![~view from the
  balcony](…)` or `![-view from the balcony](…)`. The character does not
  spoil the caption and is removed by the same shortcut (Ctrl+D on a selected
  image — `toggleTaskKey`, on an object it means "hide/show the caption");
  Enter on an image edits the caption in a field under the picture;
- a name from a camera, phone or clipboard per the `imageCaption.noname`
  regexp from the config (by default `IMG_1234`, `DSC…`, `image 3`,
  `Изображение`, `Screenshot … at …`, `0A5A0229_DxO`, a bare number of six
  or more digits).

An inline image without a caption (`before ![](x.png) after`) has nothing to
hold on to in the live document, so at build time it receives the nameless
name `image N` (N — ordinal among the paragraph's images, not required to be
in order) and goes into the file as `![image 1](x.png)`. An image that is a
whole paragraph is an object; its empty caption lives in a property and stays
empty.

**Images are not versioned.** Only the note's text goes into the history, and
in it an image is a single link.

A missing attachment does not spoil the note much: in its place a frame
`01jd….webp: file not found` is drawn, the space for it is kept, and the
bytes of the link are untouchable — the file comes back, the picture comes
back.

**An attachment's archivedness is derived, not stored** (stage 10, since
stage 15 — about the archive):

    an attachment is "in the archive" ⇔ all notes referring to it are in the archive

So when a note is put into the archive and brought back, nothing is done to
the attachment files: the images follow the note there and back by
themselves, and there is nothing here to fall out of sync. A returned note
renders its images — the files were lying in place the whole time.

Physical parting happens at exactly one moment: **"delete forever"**. For
every attachment of the notes being deleted, a byte search for its id runs
over all remaining `.md` (live and archived); found nowhere — the file goes
into the OS trash together with the notes. A false positive (an id as text
inside a code block) errs on the safe side: the file stays.

History **deliberately takes no part** in this computation (owner's
decision): the deletion is radical, there are two safeguards (archive →
"delete forever" → OS trash), and a snapshot referring to something gone
degrades into the standard "file not found" frame. Otherwise no image would
ever leave the store — the past sees it.

Hence the three categories in `verify`: a live attachment (at least one
non-archived note refers to it), "archive-only" (an informational line, not
an anomaly) and an orphan — mentioned in no note at all.

---

## 5. Edit history

`history/<id>.log` is the append-only journal of one note.
The format details and the rationale are in `store/journal.h`; here is what
one needs to know about the file on disk.

### Format

A CBOR sequence (RFC 8742). The first record is the header
`{1: "zametti-journal", 2: format version, 8: content version}`; then come
records, each a map with integer keys:

| key | what |
|---|---|
| 1 | record kind: 1 save, 2 external, 3 restore, 4 tombstone |
| 2 | UTC time, milliseconds since the epoch |
| 3 | BLAKE3 of the uncompressed snapshot, 32 bytes |
| 4 | snapshot codec: 1 zstd, 2 zstd relative to the previous snapshot |
| 5 | snapshot size before compression |
| 6 | the snapshot itself, compressed |
| 7 | for `restore` — the time of the source record |
| 8 | *(header only)* content version: which rule set the journal was cleaned by |
| 9 | revision (a Lamport counter); no key — `0`, a record written before stage 17 |

A snapshot is **the bytes of the note file in full**, header included. Not a
line diff: restoring must be simple.

### Record kinds

- `save` — an ordinary save;
- `external` — the file was changed from outside, and we saw it;
- `restore` — a person brought an old snapshot back;
- `tombstone` — the note was deleted. It has no snapshot of its own: the
  previous record remains final.

### The revision

Key 9 is the record's **revision**: a Lamport counter the journal assigns
itself on append — `seq = max(all revisions it knows) + 1`. Nobody outside
passes it in: the revision is a property of the journal, not of the intention
of whoever writes.

It exists so that the order of records stops depending on clocks. A machine
whose time ran ahead leaves a record "from the future", and by time alone that
record would stay the head until the date it claims; by revision it is beaten
by the first causally later edit made anywhere. A tombstone gets a revision
like any other record — that is exactly what makes an edit win over a deletion
that happened before it.

**No key at all means 0** — a record written before stage 17. Such records
compare among themselves by time, as they always did, and the first new edit
legitimately becomes the head. A zero revision is not written into the file, so
an old journal that goes through thinning comes out byte for byte the same.

Adding the key did not raise the format version: an unknown key is skipped
silently (the promise from stage 7), so an older build still reads a journal
written by a newer one — it just does not see the revisions.

### Generations

On disk a snapshot lies relative to its predecessor, in chains of 32 records:
each generation starts with a full snapshot, the rest are diffs (the
`zstd --patch-from` mechanism). This makes the journal four to twenty times
smaller, and the chain does not grow without bound: both the cost of reading
and the damage from corruption are limited to one generation.

The numbers on the corpus's largest note (239 KB, 200 edits): full snapshots
15.8 MB, generations of 32 — 0.57 MB. The journals of the whole corpus:
28.2 MB versus 2.1 MB.

### Content version and lazy cleaning

Key 8 in the header answers the question "has this history been cleaned",
not "will I manage to read it" — the latter is answered by the format
version, and mixing them into one number is not allowed. No key at all means
**v0**, a journal written before stage 10; `"0.1"` — the history has been
cleaned by the stage 9 rules (equivalent records do not exist, a small edit
replaces the previous one, returning to a recorded state collapses the tail).

Cleaning is **lazy and per-note**: a journal is brought to 0.1 at the first
write into it and at the first read done for the sake of this note's history
(Ctrl+Z, entering history, history search). Simply opening a note does not
touch the journal. Corpus-wide sweeps (`verify` and the future all-notes
search) **never rewrite** journals: otherwise the very first sweep would
become a global cleaning utility through the back door.

The migration differs from live writing in exactly one thing — the
**freshness guard**: live collapsing touches only fresh records (returning to
a month-old state is legitimate), migration cleans retroactively, paying no
attention to age. Untouchable in both modes: the anchor record, the
tombstone, the last record and the `external` marks, across which collapsing
never jumps.

By hand (for testing without the UI): `zametti-store history compress
<id | path>` — forces the same function and prints what came out. There is
no regular way for a person to clean the history by hand, and none is
planned.

### Journal rules

- **the journal is never deleted** — even when the note itself is deleted.
  From the last snapshot with a tombstone it can be resurrected;
- **the journal only grows.** There are two exceptions — thinning and lazy
  cleaning — and both rewrite the file whole and atomically;
- **restore is a new record.** The journal does not rewind: a restore can be
  undone, but not erased from the history;
- **fsync is not called.** The bytes will leave with the normal writeback; a
  torn tail (a crash or a power cut during a write) is a normal case: it is
  cut off on open and on the first append, and the journal is never
  considered corrupt because of its last record;
- **deduplication is free:** a save happens only when the hash changes, so
  there are never two identical snapshots in a row.

### Thinning

A logarithmic scale from the current moment:

| record age | how many we keep |
|---|---|
| the last hour | all |
| up to a day | at most one per minute |
| up to a week | one per hour |
| up to a month | one per day |
| beyond | one per month, forever |

**The last record is never thinned.** Thinning is idempotent: a repeated run
at the same "now" does not change a byte. It runs in the background at
program start and on the command `zametti-store thin`.

---

## 6. Archive

A put-away note DOES NOT MOVE. Its header gains `archived: yes`, while
`parent` stays as it was — the "Archive" in the tree is a virtual folder
assembled from the marked notes. Hence the return: remove the mark, and the
note is home, with no memory of where it was taken from.

On put-away the body departs into the journal, and the file becomes a
**stub** — the header plus the title line:

```
<!-- zametti
parent: 01n6cqevr3wprw
created: 2019-06-19T10:54:29+03:00
modified: 2026-07-30T22:26:24+03:00
archived: yes
-->

# Photo gear
```

The title in the stub is not decoration: through it the note is visible in
the list and found by name search, and the file remains legitimate markdown.
The body is no longer in the file — it is in `history/<id>.log`, and opening
an archived note shows the head of the journal in history mode: reading is
possible, editing is not.

**The put-away order is rigid, and crash-resistance grows out of it:**

1. first the journal — the body goes into the history by the common
   selection rules (equal to the head — no record; a near-duplicate replaces
   the last one);
2. then the stub — the file is replaced atomically.

A crash between the steps leaves the full note and a journal record: a retry
is idempotent, and losing the body is impossible by construction. The
reverse order would open a window in which the body exists nowhere.

**Delete forever** is the only operation that takes the journal away: an
archived note's body lives there and nowhere else, and keeping it would mean
not deleting the note but hiding it. An ordinary note's journal always
survives deletion.

**A consequence, named out loud:** search across all notes (Ctrl+Shift+F)
does not see archived bodies — they are not in the files. Titles are
searched, per-note history search works.

**The old trash bin** (`role: trash`, notes inside it with `trash-parent`)
is migrated once at the first start: `parent` := the previous parent,
`archived: yes`, the trash keys go away, the emptied trash note is deleted.
Bodies are not touched — the migration is about structure. The value
`role: trash` is still READ as archivedness: the store may have arrived from
another machine or from an older build.

---

## 7. Lost & found

A note with an **unresolvable `parent`** — the file was brought back from the
system trash behind the program's back, copied from another store, sync
delivered the child before the parent — gets registered into the special
folder `role: lost`:

    parent := Lost & found,  lost-parent := what it was

This is **the only place where loading the store WRITES** (owner's decision),
and the bounds of the exception are strict: only notes with an unresolvable
`parent`, only two header lines, `modified` is not bumped, a repeat writes
nothing, and the folder itself is created only for the first find. Things
are taken out of the bureau by an ordinary move; the folder controls nothing
else.

Id collisions between stores are declared negligible: an id is 8 characters
of time and 6 random ones — a match would mean two notes created within the
same second on two machines, and with the same CSPRNG roll on top of that.

---

## 8. Locks and concurrent access

- **one store — one program.** At start the file lock `.zametti/store.lock`
  is taken; a second copy on the same store does not start and says which pid
  holds the lock. A lock forgotten after a crash removes itself if no
  process with that pid exists any more; for the remaining cases — the
  switch `zametti --root … --unlock`;
- **inside the program** all history operations go through one shared
  in-memory lock: background thinning and note saving do not overlap;
- `zametti-store thin` honestly refuses to work while the program is open.

---

## 9. What can be done by hand

You may: read and edit the `.md` with any editor — the program will notice
the edit and record it in the history as `external`; copy the store as a
whole; put it in git.

You may not: edit `history/*.log` — it is a binary format with hashes;
create files with names outside the format (`verify` will count them as
trouble); keep one store open in two copies of the program.

Worth remembering: **deleting a note file behind the program's back leaves
no tombstone**. The journal remains, and `verify` will say "the file was
taken behind the program's back" — not trouble, but not order either.

---

## 10. The store utility

```
zametti-store init <dir>                          an empty store
zametti-store new --root <dir> [--parent <id>]    an empty note
zametti-store import --root <dir> --from <src>    import a tree of .md
zametti-store verify --root <dir>                 full check
zametti-store thin --root <dir> [--dry-run]       history thinning
zametti-store history compress <id | path.md>    clean one journal (test hatch)
```

`verify` checks: the names of files and attachments, the headers, the
absence of `serialize(parse(x))` drift, `parent` links and cycles, the
existence of `![…]`-link targets, the bodies of folder notes, and across
the journals — record framing, the format version, the reconstruction and
the hash of **every** snapshot. It divides attachments into three categories
(live, "archive-only", orphan) and never deletes them: files leave by one
single path — "delete forever". `verify` does not edit journals by a single
byte: history cleaning is per-note and lazy.

Import **never writes into the source**: the new store is created alongside,
the old tree remains the reference.
