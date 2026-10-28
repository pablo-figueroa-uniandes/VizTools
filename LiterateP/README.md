# UsdOptixViewer as a literate program

[`UsdOptixViewer-literate.pdf`](UsdOptixViewer-literate.pdf) explains every line of
the viewer's code in the style of Donald Knuth's *Literate Programming*. Code is
presented in numbered sections, in the order that best explains it, each with the
prose that motivates it. Named chunks such as ⟨Find the OptiX headers 10⟩ ≡ are
cross-referenced, and the book ends with an index of chunk names.

The book covers the CMake project, the shared launch parameters, the scene, the
OptiX renderer, the device programs, the application and the XR output (OpenXR
and CloudXR). The third-party `stb_image.h` and `cxrServiceAPI.h` are not covered.

## Files

| Path | Contents |
|------|----------|
| `src/*.nw` | The book, in [noweb](https://www.cs.tufts.edu/~nr/noweb/) syntax, one file per part, read in name order |
| `lp.py` | Tangles, checks and weaves the book |
| `preamble.tex` | Typesetting conventions (CWEB look), title page |
| `build.sh` | Checks, weaves and typesets the PDF |
| `UsdOptixViewer-literate.pdf` | The typeset book |

## The book and the code stay identical

```bash
python3 lp.py check
```

`check` tangles the book in memory and compares every root chunk, byte for byte,
with the file of the same name in the repository. It fails if any source file
has no root chunk. Run it after changing either the code or the book.

Other commands:

```bash
python3 lp.py tangle /tmp/out     # write the tangled sources
./build.sh                        # check + weave + typeset the PDF
LP_ONLY=30-scene.nw python3 lp.py check   # work on one part
```

Typesetting needs Python 3 with Pygments, and [Tectonic](https://tectonic-typesetting.github.io),
a self-contained TeX engine that downloads the LaTeX packages it needs on first use.

## Writing conventions

- `@** Title.` starts a part, `@* Title.` a titled section, and `@` a plain
  section. Prose is LaTeX, with `[[code]]` for inline code and `<<Chunk name>>`
  to cite a chunk.
- `<<Chunk name>>=` on its own line starts a code chunk. A line that holds only
  `<<Other chunk>>` includes that chunk, indented like the reference.
- Chunks whose names are never referenced are roots. Each root is named after
  the file it produces.
