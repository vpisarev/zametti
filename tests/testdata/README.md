# Public test fixtures

Everything in this directory is committed and safe to publish. Tests look for
a fixture here first and fall back to the private, git-ignored `.testdata/`
next to the sources (see `tests/testdata.h`; `ZAMETTI_TESTDATA` overrides
both). A suite whose fixture is missing everywhere says so out loud and passes
empty — it never pretends to be green.

Rule: only synthetic notes and public texts go here. Copies of anyone's real
notes, photos or screenshots stay in `.testdata/`.

| Entry | What it is | Read by |
|---|---|---|
| `commonmark/NNNN.md` | The markdown input of every example in the [CommonMark spec](https://spec.commonmark.org/) (655 examples), one file per example | `ZDocument.IdempotentOnForeignMarkdown`, `Table.All` |
| `gfm/NNNN.md` | Same for the [GitHub Flavored Markdown spec](https://github.github.com/gfm/) (672 examples) | same |
| `Typesetting Math in Texts.md` | A short article on writing math in markdown, plain markdown without a zametti header | `MathIr`, `MathScan`, `DocumentRoundtrip` |
| `typesetting-math.md`, `formula-shift/typesetting-math.md` | The same article stored as a zametti note (header with parent id and dates) | `InlineFormula*`, `BlockGeometry` |
| `search-steps-fixture.md` | A small note with a poem, a task list, an image reference and two code blocks | `Search.*`, `MarkdownEdit.*` |
| `history-bench/` | A one-note synthetic storage with a history journal | `zametti-bench` (history) |

## Regenerating the spec examples

The `commonmark/` and `gfm/` trees are produced from the upstream `spec.txt`
files by `tests/extract_spec.py`; the script keeps only the markdown side of
each example and turns `→` back into tabs:

```
tests/extract_spec.py path/to/commonmark/spec.txt tests/testdata/commonmark
tests/extract_spec.py path/to/gfm/spec.txt        tests/testdata/gfm
```

The specifications are © John MacFarlane and GitHub, licensed
[CC-BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/); the example
excerpts here are redistributed under that license.
