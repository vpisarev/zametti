# Storage Organization

This document describes what lies on disk: how the note store is organized,
what a note file consists of, what an attachment is and how the history is
recorded. It is a reference for the format, not for the code; everything said
here is checked by `zametti store verify` and by the test suites.

The main property: **the store can be read and edited without the program**.
A note is a plain markdown file in UTF-8, the history is a file next to it.
Should the program vanish — the texts remain.

---

## 1. The directory

The store is flat. No nested folders: the hierarchy lives in the metadata,
not in the file system.

```
store/
    zametti.json               the store identity: who this store is
    01n6cqevh7bbfr0a.md        a note
    01n6cqevsd7v5edf.md        a note
    01jd7f0kq2m8xab7.webp      an attachment
    history/
        01n6cqevh7bbfr3v.log   edit history of this note
    .zametti/
        store.lock           the lock: one store — one program
        last-written         the device time floor (journal stamps)
        remote.json          the cloud address of THIS copy (no secrets)
        dirty                names touched since the last sync (write-ahead)
    .rescue/
        …                    buffers that failed the write self-check
```

Reserved names and their fate under synchronization:

| name | what it is | synchronized |
|---|---|---|
| `zametti.json` | the store identity | **yes**: in the cloud it is the manifest |
| `history/` | edit history | **yes**: it is data, not a cache |
| `.zametti/` | store state | no |
| `.rescue/` | rescued buffers | no (dot-prefixed, local) |

### `zametti.json` — the store identity

```json
{ "storeId": "01n6cqevh7bbfr", "formatVersion": 2,
  "created": "2026-08-23T00:00:00+03:00", "rootNote": "01n6cqevsd7v5e" }
```

- `storeId` — minted **once**, when the store is created, and never changes.
  Same alphabet as note ids: one look tells you it is ours;
- `formatVersion` — the version of the DIRECTORY layout (not of a note and not
  of a journal). A version newer than the program's own means: do not work,
  rather than corrupt. A build that does not know half the keys would rewrite
  the file without them and lose data silently. Unknown keys **within** a known
  version, on the contrary, survive the rewrite. Version 2 = the cloud blob
  extensions of our own (`<id>.zm`, `<id>_<ext>.pic`, see §11); version 1 files
  are still read;
- `created` — when the store was created, ISO-8601 with an offset;
- `rootNote` — id of the root note (see below). The only field that changes.

It lives **next to the data**, not in the settings: a copy of the directory
must know whose cloud it is, and in the cloud this same file serves as the
manifest — "is this the same store" is answered BEFORE a password is entered
and before a single blob is decrypted.

The file may be missing: that is what a store created by an older build looks
like. It is minted on the first run (`zametti store root init` does it too),
and `verify` says so out loud rather than treating it as trouble.

### The root note

The top row of the tree is a REAL NOTE with `role: root`, not a caption:

- its heading is the **name of the store**, and renaming that row with F2 is
  how the name changes;
- its `sort` mark is the order of "all notes" — the order belongs to the store
  and travels with it, rather than staying on one machine;
- it is a folder (`isFolder()` is true for it), so the rule "a folder's body is
  exactly one heading" and all the tree rules apply to it without exceptions;
- `parent` of root-level notes stays EMPTY. They are not re-parented onto the
  root note: rewriting 282 headers would mean 282 journal records and a first
  sync at the price of the whole store — and the Lost & found rests exactly on
  the difference between "empty parent" and "unresolvable parent";
- it cannot be archived (its subtree is the whole store), deleted, moved or
  stripped of its role. Renaming is allowed — that is the point of it.

Its address is written in `zametti.json`, and its role is in its own header:
two independent records of the same fact. When they disagree —
`zametti store root fix` names in the json whatever was found by role.

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
| `role` | `folder` — a folder note, `lost` — the Lost & found, `root` — the store's root note (exactly one per store) |
| `archived` | `yes` — the note is put away into the Archive; no key — alive |
| `lost-parent` | the find's previous parent; set by the Lost & found |
| `access` | `read-only` — the note may be read but not written; no key — an ordinary, editable note. On a FOLDER the mark covers everything inside it |
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
- **`access: read-only` is inherited downward, like `sort`.** A note is locked
  either by its own mark or by the mark of any folder above it, so a whole
  imported library is locked by one line in one folder header. What the lock
  means: the program does not rewrite the file, does not archive it, does not
  delete it, does not move or rename it, and shows no caret over it — the
  window greys the commands out and the store refuses them anyway, each on its
  own. The one edit a locked note survives is the removal of its own lock:
  a one-way lock would be a trap, not a safety. An unknown value of the key is
  not an error — the note counts as editable, the key in the file is left
  untouched, and the program says in stderr what it did not understand;
- `sort` on the ROOT note is the order of "all notes"; on any other folder it
  is the order inside that folder. A label of the FOLDER: how its notes and
  subfolders are ordered.
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

**The note's history does not version images.** Only the note's text goes into
`history/<id>.log`, and in it an image is a single link. The image's own
version lives **in the file**, in XMP:

- `zametti:Rev` — a revision, born as `1` when the image is imported and
  growing on **any** change to the picture: a retouch, a re-compression, added
  geotags or keywords. So `Rev == 2` means "this picture was changed", not
  "this picture was deleted";
- `zametti:Deleted` — a separate flag, set when the image is buried (below).

The two are orthogonal on purpose. Attachments have no journals, and the file
is the only thing that travels between machines; the bookkeeping can be lost,
and then there is nothing to compare two copies of a picture by except the
copies themselves. When they disagree, the larger `Rev` wins — the same rule
for a retouch and for a burial.

A file taken into the store **as it is** (the byte-exact JPEG→JXL transcode, a
JXL we decided not to touch) carries no mark, and its absence honestly reads as
revision `1`: the file on disk is the first version. Prising open a container we
deliberately do not rebuild would be worse.

A missing attachment does not spoil the note much: in its place a frame
`01jd….webp: file not found` is drawn, the space for it is kept, and the
bytes of the link are untouchable — the file comes back, the picture comes
back.

**An attachment's archivedness is derived, not stored** (stage 10, since
stage 15 — about the archive):

    an attachment is "in the archive" ⇔ all notes referring to it are in the archive

So when a note is put into the archive and brought back, nothing is done to
the attachment files: the images follow the note there and back by
themselves, and there is nothing here to fall out of sync.

### Burial: a deleted attachment is not erased

**"Delete forever"** is still the one moment when an attachment parts with the
notes. For every attachment of the notes being deleted, a byte search for its id
runs over all remaining `.md` (live and archived); found nowhere — the file is
**buried**. A false positive (an id as text inside a code block) errs on the
safe side: the file stays as it was.

Burying is not deleting. In place of the file there remains a **mini preview**:
the same picture downscaled by the area rule (`images.maxDeletedImageSize`,
`100` by default — that is, at most 10 000 pixels), carrying `zametti:Deleted`
and one more revision. Erasing would have been simpler and wrong: a deletion has
to **reach** the other machines, and "there is no file" cannot travel. To say
"it is gone" one needs a file that says so.

The preview is always JXL — we write exactly one format — so `<id>.webp` becomes
`<id>.jxl` and the original goes into the OS trash. Burying is idempotent: an
already marked file is not touched by a single byte, or the revision would grow
for ever and the picture would be squeezed again on every pass. An image we
cannot read at all (a foreign format, a broken file) falls back to the old
behaviour — the OS trash — and the reason is said out loud.

The note's history **deliberately takes no part** in the computation (owner's
decision): the deletion is radical, there are two safeguards before it (archive
→ "delete forever"), and a snapshot referring to a buried image shows its ghost
instead of the photo. Otherwise no image would ever leave the store — the past
sees it.

Hence the four categories in `verify`: a live attachment (at least one
non-archived note refers to it), "archive-only" (an informational line, not an
anomaly), **a deleted one** (nobody refers to it, but it carries the mark — that
is a headstone, not an orphan, and it belongs in the store) and an orphan —
mentioned in no note at all and carrying no mark.

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
| 10 | the records this one voids: an array of `[time, fingerprint]` pairs; no key — it voids nothing |
| 11 | checksum of the frame: BLAKE3, the first 16 bytes; no key — a record written before stage 17 |

A snapshot is **the bytes of the note file in full**, header included. Not a
line diff: restoring must be simple.

### Record kinds

- `save` — an ordinary save;
- `external` — the file was changed from outside, and we saw it;
- `restore` — a person brought an old snapshot back;
- `tombstone` — the note was deleted. It has no snapshot of its own: the
  previous record remains final;
- `amendment` — no new content, only a list of voided records. This is how "I
  typed something and undid it" is written down: the person came back to a
  state that is already in the journal, so there is no new snapshot to write,
  yet the records in between have to be declared out of the count.

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

### Voiding: the journal does not erase, it declares out of the count

A small edit does not deserve a waypoint of its own, and neither does a stretch
of work that the person undid on the spot. Both used to be handled by cutting
the tail of the file off. That works on one machine and stops working the
moment there are two: cutting leaves no trace, so a record that had already
travelled comes back at the next merge — and comes back for ever, because
nothing in the cloud says why it went away.

So the tail is not cut on its own any more. The new record **names** the ones
that are no longer in the count, by the same address the merge uses — time plus
fingerprint (key 10). What that gives:

- the bytes of a voided record are dropped locally right away — if they lie at
  the end of the file it is simply shortened, otherwise the file is rebuilt;
- any machine that sees the new record voids the same records at its end, so
  the cloud copy loses them too;
- a record that comes back from an older copy is voided again and never
  surfaces.

Voided records are shown nowhere — not in the timeline, not in the search over
history, and they are never chosen as the head. A record that voids without
adding content is an `amendment`.

**The address is a name, not a threshold**, and that is deliberate twice over. A
threshold ("void everything below revision N") would also void a concurrent
edit from another machine that happened to fall below it — that is, it would
break "an edit beats a deletion". And a threshold is catastrophically sensitive
to corruption: one flipped bit turns 16 into 4096 and mows down a range, while a
damaged address matches nothing at all and is simply not applied — to hit
another live fingerprint one would have to search 2^256.

### Integrity, and what can actually corrupt a record

The fingerprint (key 3) covers **the snapshot** and nothing else. The kind, the
time, the revision, the source, the layout and the list of voided records were
covered by nothing at all — and a record's kind is a single byte. Two flipped
bits turn a save (1) into a tombstone (4); the snapshot is then ignored, the
note looks deleted, and the deletion travels to the other machines as a
perfectly legitimate one. The mass-deletion safeguard does not fire either:
it is one note.

Hence key 11, the checksum of the frame. It is computed when a record is
written and verified when it is read, both through the same function — if the
two sides disagreed about what is covered, the checksum would start lying.

Three paths of corruption and what catches each:

- **on the wire and in the cloud** — the AEAD tag of the encrypted blob
  (XChaCha20-Poly1305, 128 bits; the chance of corruption passing unnoticed is
  about 2⁻¹²⁸). A blob is rejected **whole**, never decrypted into "almost the
  same thing"; the format version, the blob type, the store id and the blob name
  go into the associated data, so a swapped or mixed-up blob is caught by the
  same tag;
- **on disk** — the drive's own ECC; a silent flip is a few per 10¹⁵…10¹⁶ bits
  read, which on a half-megabyte journal is about 10⁻⁹ per read;
- **in memory without ECC** — the dominant term, and the only one that matters
  here, because it happens **before** encryption: the AEAD will faithfully sign
  whatever it is given. That is exactly what key 11 is for, and it is why
  integrity has to be checked before uploading rather than after downloading.

A record whose checksum does not match **stays where it is** and is skipped: it
is not shown, not chosen as the head, and not used as a link in a delta chain,
while its neighbours are read as usual. One flipped bit must not cost the rest
of the journal. (A torn tail is a different thing: there the record is simply
not there, and the tail is cut.) `verify` reports such records as trouble.

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

By hand (for testing without the UI): `zametti store history compress
<id | path>` — forces the same function and prints what came out. There is
no regular way for a person to clean the history by hand, and none is
planned.

### Journal rules

- **the journal is never deleted** — even when the note itself is deleted.
  From the last snapshot with a tombstone it can be resurrected;
- **the journal only grows.** The file does get shorter — but never on its own:
  only as a consequence of voiding (see above), and in the two whole-file
  rewrites, thinning and lazy cleaning, both of which are atomic;
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
program start and on the command `zametti store thin`.

---

## 6. Archive

A put-away note DOES NOT MOVE, and **nothing is taken out of it**. Its header
gains one line, `archived: yes`, while `parent` stays as it was — the "Archive"
in the tree is a virtual folder assembled from the marked notes. Hence the
return: remove the line, and the note is home, with no memory of where it was
taken from. Restoring does not need the journal at all — the body never left the
file, so a note that arrived from another machine without its history comes back
just the same.

The order is: **first the file, then the journal**. The journal entry is written
by the ordinary rules of selection — the mark is a small change, so it voids the
previous record rather than standing next to it — and the head of the journal
must agree with the file. That agreement is what the whole synchronization
stands on.

Archiving **parses nothing** (owner's rule). A note may be broken by anything —
an edit in a foreign editor, a bad disk, a mistake of ours; as long as the
header is in place, the program is obliged to put it away. That is also why
archiving cannot be reduced to the ordinary rewrite path: that one parses.

### Why it used to be different

Until stage 17 archiving **cut the body out** into the journal, leaving a stub
of the header plus the title line. The saving on disk was considerable, and it
was paid for four times over, silently every time:

- along with the body, the **links to attachments** left the file — and it is by
  those links that we count what should go when a note is deleted for ever. The
  images of archived notes were therefore never deleted at all;
- an image held **only** by an archived note was taken away by the deletion of a
  *neighbouring* one: a stub did not "hold" it;
- search across the store stopped seeing archived bodies — they were not in the
  files;
- a person leaving history mode got the stub, fully editable, and the first save
  put the stub into the journal on top of the body.

A store that lived through those builds is **unfolded on opening**: the body
comes back from the head of the journal into the file, the mark stays. The
header is taken from the file (it holds `parent`, `sort` and everything a person
could have changed while the note lay in the archive), `modified` is not
touched, nothing is written into the journal. The stub detector is byte-based
and conservative — a body counts as a stub only if, after dropping empty lines,
at most one non-empty line is left and it starts with `#`. In doubt, don't
touch: a mistake here means a damaged note.

### An archived note is shown, not edited

It opens in an ordinary view, as WYSIWYG or as source — with the body, the
images and everything else. Two things differ: it cannot be edited until it is
brought back, and the field is tinted with the same grey as history
(`colors.historyBackground`). There is no caret in a view, and that is all a
person needs to be told — no banners and no warnings (owner's decision; the
"About" window works the same way).

Search across the store finds archived bodies now that they are in the files.
Such hits are **marked** and go **after** all the live ones: one searches among
what one uses, and the put-away is an answer to "wasn't it somewhere else?".

### Delete forever

The path is one and the same for archived and live notes: a tombstone into the
journal, the file into the OS trash, the subtree if it is a folder, and the
exclusive attachments buried (see §4).

**The journal survives, always.** It is the only carrier of the fact of the
deletion: the note is gone, the file is gone, and the only thing that can tell
the other machines "it is no more" is the tombstone. Taking the journal away
would not delete the note but hide it locally — the first sync would bring it
back from any machine where it is still whole.

What the journal keeps is the **last snapshot and the tombstone**: a full
history after two deliberate decisions in a row (into the archive, then delete
from the archive) is dead weight, and the last state is enough to bring the note
back. The tombstone names the voided records, so the other machines' copies of
the journal lose them too rather than growing back through the merge.

Bringing it back is `zametti store resurrect <id>`: the snapshot is written into
the file as it is, and since it carries `archived: yes` — the note was deleted
*from the archive* — the note returns **into the archive**. Exactly one of the
two decisions is undone; whether to take it out of the archive is for the person
to decide. Its images are the buried ones, downscaled.

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

This is one of the **three sanctioned exceptions to "loading the store never
WRITES"** (owner's decision; the full list lives in `ZStorage`:
`ensureIdentity` mints `zametti.json`, `ensureRootNote` creates the root
note, and `migrate()` runs the idempotent migrations — old trash → archive,
unfolding archived stubs of older builds, and this bureau). The bounds of the
bureau exception are strict: only notes with an unresolvable `parent`, only
two header lines, `modified` is not bumped, a repeat writes nothing, and the
folder itself is created only for the first find. Things are taken out of the
bureau by an ordinary move; the folder controls nothing else.

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
- `zametti store thin` honestly refuses to work while the program is open.

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

## 10. The store from the command line

The store utility is not a separate program: it is the `store` subcommand of
`zametti` itself. (It was a separate `zametti-store` binary until 2026-08-26;
both binaries carried a full copy of every vendored library, so a distribution
was twice the size it had to be.)

```
zametti store init <dir>                          an empty store
zametti store new --root <dir> [--parent <id>]    an empty note
zametti store import --root <dir> --from <src>    import a tree of .md
zametti store verify --root <dir>                 full check
zametti store thin --root <dir> [--dry-run]       history thinning
zametti store history compress <id | path.md>     clean one journal (test hatch)
zametti store recompress --root <dir> --id <id|all>   re-encode attachments
zametti store resurrect --root <dir> --id <id>        bring a deleted note back
zametti store root show|init|fix --root <dir>         the identity and the root note
zametti store set-remote --root <dir> (--url <dav>|--to <dir>) [--reset]
zametti store sync --root <dir> [--full|--push-only]
                  [--allow-mass-delete|--keep-all]    the engine, see §12
```

`zametti store --help` prints the same list with the options spelled out.

`root show` prints the identity and both records of the root note's address —
the one in `zametti.json` and the one found by role — and says `MISMATCH` when
they disagree. `root init` and `root fix` are the same action from two sides:
find the root note or create it, then name it in the json. Both go through one
`ZStorage::ensureRootNote` — there is no parallel implementation.

`verify` checks: the names of files and attachments, the headers, the
absence of `serialize(parse(x))` drift, `parent` links and cycles, the
existence of `![…]`-link targets, the bodies of folder notes, and across
the journals — record framing, the format version, the reconstruction and
the hash of **every** snapshot. It divides attachments into four categories
(live, "archive-only", deleted — a mini preview carrying the mark — and orphan)
and never touches them: an attachment leaves by one single path, "delete
forever", and even then it is buried rather than erased. `verify` does not edit
journals by a single byte: history cleaning is per-note and lazy. A record whose
frame checksum does not match is reported as trouble.

Import **never writes into the source**: the new store is created alongside,
the old tree remains the reference.

---

## 11. The cloud: blobs and the keyfile

The cloud stores ONLY journals + attachments + the keyfile (plus the open
`zametti.json` manifest — see §1). Everything encrypted travels as blobs
sealed with one 32-byte random **master key**; the keyfile is that key
wrapped with the user's password.

### The blob wrapper, v1

Names in the cloud are flat and open (the owner's decision — ids are
opaque): `<id>.zm` for journals, `<id>_<ext>.pic` for attachments, plus the
open `keyfile` and `zametti.json`. The extensions are our own (the owner's
decision, 28.08.2026): an encrypted blob under a real image extension used
to look like a corrupted picture to hosting services, so the real extension
moved INTO the name — and burying an attachment (webp → jxl preview) stays
a rename in the cloud, propagating through the presence model as before.
Legacy names (`<id>.log`, `<id>.<ext>`) are still read while they exist;
a sync run re-seals such a blob under the new name (the AAD includes the
name, so a server-side rename is impossible), deletes the legacy one only
after the upload, counts it as `migrated` in the report, and re-uploads
the manifest at the current `formatVersion` — a fence that stops older
builds honestly (`tooNew`) instead of letting them push legacy names back.
The bytes of every encrypted blob:

| offset | size | what |
|---|---|---|
| 0 | 4 | magic `"ZBLB"` |
| 4 | 1 | wrapper version = 1 |
| 5 | 24 | nonce — random, fresh **for every upload** |
| 29 | n+16 | XChaCha20-Poly1305 ciphertext with the tag attached |

The plaintext is the file as it is on disk — a journal is already
zstd-compressed inside, an attachment is already JXL/WebP/JPEG — so there is
no compression layer here.

AAD of the seal:
`"zametti-blob\0" + version + "\0" + ("journal"|"attachment") + "\0" +
storeId + "\0" + blob name`. So a valid blob copied under another name,
into another store, presented as another kind, or with the version byte
rolled back fails the tag instead of decrypting into something plausible.
The overhead is 45 bytes per blob; a fresh nonce per upload means the same
content uploads as different bytes every time, and nonce reuse is excluded
by birth rather than by bookkeeping.

In the program: `BlobCipher` (interface) / `XChaChaCipher`
(`zametti-core/sync/blob_cipher.h`); the cipher is born from a `Keyfile`
with a live key and holds it as a `Keyfile` — raw key bytes do not travel.

### `keyfile`

Lives in the cloud next to the blobs, under this very name, in the open —
its etag lets a device notice a rotation or a password change before
decrypting anything. JSON + base64, human-readable on purpose:

```json
{ "version": 1, "storeId": "01n6cqevh7bbfr",
  "created": "2026-08-24T12:47:13+03:00",
  "kdf": "argon2id13", "opslimit": 4, "memlimit": 536870912,
  "salt": "base64(16)",
  "cipher": "xchacha20poly1305-ietf", "nonce": "base64(24)",
  "key": "base64(32+16)" }
```

- `key` is the master key sealed with XChaCha20-Poly1305; the sealing key is
  `Argon2id(password, salt)` with the `opslimit`/`memlimit` **written right
  here**: a future device reads the parameters from the file, not from its
  own constants, so the defaults may change without breaking old keyfiles.
  The defaults themselves are the owner's threshold (512 MiB × 4 passes,
  ~1.5 s on his machine, calibrated by `zametti-bench argon2`);
- the AAD of the seal is `"zametti-keyfile\0" + version + "\0" + storeId`:
  downgrading `version` in the text or moving the envelope to another store
  fails the tag instead of silently changing the parse;
- a failed unwrap means "wrong password or a corrupted keyfile" — the two are
  indistinguishable by construction of AEAD. A *rotation* is told apart
  differently: the keyfile unwraps fine, but incoming blobs do not decrypt;
- `version` newer than the build understands — a polite refusal ("update the
  program"), not corruption. Unknown keys within a known version survive a
  rewrite, same promise as everywhere;
- changing the password = re-wrapping the same key with a fresh salt and
  nonce (`rewrap`); rotating the key = a new key plus a full re-upload.

In the program the keyfile is `Keyfile` (`zametti-core/sync/keyfile.h`) — a
pure value, no files; it is also the ONLY carrier of the live master key:
raw key bytes never travel on their own.

---

## 12. Synchronization

The engine runs the same four steps every time, in this order and with no
exceptions: **align → exchange → merge → materialize**. Interrupting it at any
point is safe: journals only grow, every file write is atomic, and the next
run simply continues. The cloud address, the bookkeeping and the dirty set
described below are all *around* the data — deleting any of them changes the
cost of a run, never its result.

### The address of the cloud: `.zametti/remote.json`

Belongs to THIS COPY of the store (a `cp -r` takes it along, the cloud never
sees it) and carries no secrets:

```json
{ "remoteUrl": "https://dav.example/зам/01n6cqevh7bbfr/", "remoteUser": "vp",
  "timeoutMs": 30000 }
```

`remoteDir` instead of `remoteUrl` points at a local folder used as a cloud
(tests, a mounted NAS). The legacy keys `url`/`dir`/`user` are still read and
migrate to the new names on the next write. `timeoutMs` is an INACTIVITY
watchdog: the transfer is aborted when no bytes move for that long — a large
file may take as long as it takes. The secrets live in the system keyring:
the server password as `zametti-webdav-<storeId>`, the master key as
`zametti-key-<storeId>`. The encryption password itself is stored NOWHERE —
it lives for the one moment Argon2id unwraps the keyfile, and what reaches
the keyring is the key.

`zametti store set-remote` writes all of this once; `--reset` forgets it.
Pointed at a directory that does not exist yet (or is empty), it creates the
store skeleton WITHOUT minting an identity and inherits the identity from the
cloud manifest; the root note is fetched and materialized right away, and a
one-line summary of the cloud is printed (notes, attachments, size — sums
only, never a listing). Exactly ONE side must have substance (the owner's
decision): an existing store against an empty cloud is the first device; an
empty directory against a cloud with a manifest is a new device; **empty on
both sides is refused** — in real life that is what a mistyped cloud address
or local path looks like, and silently minting a fresh store+cloud pair would
hide the typo (a fresh store starts with `init`). A non-empty directory that
is not a store is refused too, and a refusal leaves no half-made directories
behind.
The next `sync` downloads everything — a bootstrap is an ordinary sync with
an empty local side, there is no separate restore code. In the program the
same entry is `ZStorage::initFromRemote` — the future first-run dialog (and
the Android port) call it, not a parallel implementation.

### The dirty set: `.zametti/dirty`

Aligning costs O(changes), not O(store), because the write paths NAME what
they touch: before bytes are written, the note's id is appended to this file
(one name per line, no fsync — same reasoning as the journal). A run checks
the named notes, plus one cheap readdir comparing (mtime, size) against the
bookkeeping — that catches edits made with vim while the program was closed,
and deletions too. A lost mark costs one stat-scan; a stale mark costs one
hash check. After an unclean shutdown the scan runs on every platform: it
closes the window between the write-ahead mark and the write itself.

### The bookkeeping: `sync-state-<storeId>-<pathhash>.json`

Per-device CACHE in the application config directory (never in the store):
for every blob — the etag of the last operation, BLAKE3 of the uploaded
ciphertext, BLAKE3 of the local journal bytes at that moment; for every note
file — (mtime, size) for the stat-scan. Named by store id plus a hash of the
local path, so two copies of one store on one machine do not share it.
Deleting it degrades the next run to "download and merge" with the same
result (invariant D). The steady state costs one listing and nothing else:
on the owner's store — 2 requests, zero blob reads, zero uploads.

### Exchange: three tiers per journal

1. server etag matches the recorded one AND the local bytes match — skip,
   zero traffic;
2. etag differs → GET; the ciphertext hash matches the recorded one — the
   server merely re-issued the etag (a WebDAV habit), record it and move on;
3. one side changed → take or push the file WHOLE, byte for byte — this is
   what makes two stores converge to identical bytes;
4. both changed → a full merge.

**Integrity comes before classification.** A local journal that fails
validation (a damaged frame, an unreadable record) is treated as ABSENCE: the
remote copy is taken whole and NOTHING is uploaded — local bit-rot never
travels to the cloud. A torn tail is valid as always, but only the whole part
is uploaded. Symmetrically, a blob missing on the server while the
bookkeeping remembers its etag is server damage, healed by re-uploading; a
blob that fails its AEAD tag is healed the same way — unless the keyfile's
etag has changed too, which means the key was rotated on another device: the
run stops and asks for the password instead of "healing" what it cannot read.

### Merge

A pure function over two sets of records (`ZJournal::mergedWith`):

- union by the record address (time + fingerprint); records equal in address
  and identity are one record; bodyless records (tombstone, amendment) with
  the same address are different records and both live;
- the order of the result is the canonical record order (revision, time,
  content-before-tombstone, fingerprint) — hence merge(A,B) равно merge(B,A)
  byte for byte;
- voiding applies across the union: voided records are dropped physically,
  the voiding records keep their addresses — a record coming back from an
  old copy is voided again and never surfaces;
- the identity of a journal is BLAKE3 of the canonically sorted record
  identities (the frame WITHOUT its layout): re-laying generations does not
  change history. Equal sets are not uploaded and not rewritten, no matter
  how the zstd bytes differ — this is what stops upload ping-pong;
- **the merge proves itself on every call**: every input record must be found
  in the result by address or be voided by it, or the merge refuses to hand
  out its result. The merged journal proves itself again before touching the
  disk: serialized, parsed back, every snapshot rebuilt and checked against
  its fingerprint. Not proven — the file is not touched by a byte.

### Materialize

The head of the merged journal becomes the `.md` file — by the normal atomic
write, and the written bytes are read back and checked against the record's
fingerprint. The condition is double: the head differs from the file AND the
file has not changed since the align step — if it has, the note is re-aligned
instead of overwritten. An edit made while the sync was running never loses.
A tombstone head removes the file (the journal stays, as always). A run that
wants to remove more notes than the guard allows (20 by default) removes
NONE of them and reports the list; the person either confirms — the next run
applies the deletions — or refuses, and the notes are declared alive: their
files produce records ON TOP of the tombstones, the cloud heals, and the
question does not repeat because the state itself has changed.

### Attachments

Presence sync: missing in the cloud — upload, missing locally — download
(AEAD + hash checked). Both sides present and different, and neither matches
the recorded hash — a true concurrent replacement: nothing is overwritten,
the name is reported. (The `Zametti:Rev` mechanism from the brief is the next
stage; until then the engine refuses to guess.)

