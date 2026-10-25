#!/usr/bin/env python3
"""Minimal literate-programming tool for the UsdOptixViewer book.

The book is written in noweb syntax, split over src/*.nw (read in name order):

    @ Documentation in LaTeX, with [[inline code]] and <<chunk name>> references.
    @* Title. Starts a major group (a starred section, as in CWEB).
    @** Title. Starts a part: new page, large heading.
    <<chunk name>>=
    code lines; a line holding only <<other chunk>> includes that chunk,
    indented like the reference.

Every "@" line starts a new numbered section. Chunks that are never referenced
are roots; their name is the path of the file they produce, relative to the
repository root.

    lp.py tangle DIR   write the root chunks below DIR
    lp.py check        tangle in memory and compare with the repository files;
                       also verify that every source file of the project is
                       covered by a root chunk
    lp.py weave OUT    write the typeset LaTeX book to OUT

Set LP_ONLY to a comma-separated list of src file names to work on part of
the book (check then reports the other files as missing).
"""

import os
import re
import sys
from pathlib import Path

from pygments.lexers import BashLexer, CMakeLexer, CppLexer
from pygments.token import Comment, Keyword, Name, Number, Operator, Punctuation, String, Text, Whitespace

HERE = Path(__file__).resolve().parent
REPO = HERE.parent

# Source files the book must cover (third-party code is excluded).
COVERED = [
    "UsdOptixViewer/CMakeLists.txt",
    "UsdOptixViewer/cmake/EmbedFile.cmake",
    "UsdOptixViewer/src/LaunchParams.h",
    "UsdOptixViewer/src/Scene.h",
    "UsdOptixViewer/src/Scene.cpp",
    "UsdOptixViewer/src/OptixRenderer.h",
    "UsdOptixViewer/src/OptixRenderer.cpp",
    "UsdOptixViewer/src/devicePrograms.cu",
    "UsdOptixViewer/src/CloudXrService.h",
    "UsdOptixViewer/src/CloudXrService.cpp",
    "UsdOptixViewer/src/XrOutput.h",
    "UsdOptixViewer/src/XrOutput.cpp",
    "UsdOptixViewer/src/App.h",
    "UsdOptixViewer/src/App.cpp",
    "UsdOptixViewer/src/main.cpp",
    "UsdOptixViewer/src/StbImage.cpp",
    "install_all_users.sh",
]

SECTION_RE = re.compile(r"^@(\*{0,2})(?:\s|$)(.*)$")
DEF_RE = re.compile(r"^<<(.+?)>>=\s*$")
REF_RE = re.compile(r"^(\s*)<<(.+?)>>\s*$")


class LpError(Exception):
    pass


# ---- parsing ------------------------------------------------------------------
class Chunk:
    def __init__(self, name, lines, section, part):
        self.name, self.lines, self.section, self.part = name, lines, section, part


class Section:
    def __init__(self, number, level, title, part, line):
        self.number, self.level, self.title, self.part, self.line = number, level, title, part, line
        self.doc = []
        self.chunks = []


def strip_blank_edges(lines):
    while lines and not lines[0].strip():
        lines.pop(0)
    while lines and not lines[-1].strip():
        lines.pop()
    return lines


def parse(files):
    sections, chunk = [], None
    for path in files:
        current = None
        for lineno, raw in enumerate(path.read_text().split("\n"), 1):
            where = f"{path.name}:{lineno}"
            m = SECTION_RE.match(raw)
            if m:
                level, rest = len(m.group(1)), m.group(2)
                title = ""
                if level:
                    title, _, rest = rest.partition(".")
                    title = title.strip()
                current = Section(len(sections) + 1, level, title, path.name, where)
                current.doc.append(rest)
                sections.append(current)
                chunk = None
                continue
            m = DEF_RE.match(raw)
            if m:
                if current is None:
                    raise LpError(f"{where}: code chunk before the first section")
                chunk = Chunk(m.group(1).strip(), [], current, path.name)
                current.chunks.append(chunk)
                continue
            if chunk is not None:
                chunk.lines.append("@" + raw[2:] if raw.startswith("@@") else raw)
            elif current is not None:
                current.doc.append(raw)
            elif raw.strip():
                raise LpError(f"{where}: text before the first section")
    for s in sections:
        for c in s.chunks:
            strip_blank_edges(c.lines)
    return sections


class Book:
    def __init__(self, files):
        self.sections = parse(files)
        self.defs = {}  # name -> [Chunk]
        for s in self.sections:
            for c in s.chunks:
                self.defs.setdefault(c.name, []).append(c)
        # chunks continued across part files are almost certainly name clashes
        for name, chunks in self.defs.items():
            parts = {c.part for c in chunks}
            if len(parts) > 1:
                raise LpError(f"chunk <<{name}>> is defined in several files: {sorted(parts)}")
        self.uses = {name: [] for name in self.defs}  # name -> [section numbers]
        for s in self.sections:
            for c in s.chunks:
                for line in c.lines:
                    m = REF_RE.match(line)
                    if m:
                        ref = m.group(2).strip()
                        if ref not in self.defs:
                            raise LpError(f"section {s.number} ({s.part}): undefined chunk <<{ref}>>")
                        if s.number not in self.uses[ref]:
                            self.uses[ref].append(s.number)
        self.roots = [n for n in self.defs if not self.uses[n]]

    def number(self, name):
        return self.defs[name][0].section.number

    # ---- tangle -------------------------------------------------------------
    def expand(self, name, indent="", stack=()):
        if name in stack:
            raise LpError(f"recursive chunk <<{name}>>")
        out = []
        for chunk in self.defs[name]:
            for line in chunk.lines:
                m = REF_RE.match(line)
                if m:
                    out += self.expand(m.group(2).strip(), indent + m.group(1), stack + (name,))
                else:
                    out.append(indent + line if line.strip() else "")
        return out

    def tangle(self):
        return {root: "\n".join(self.expand(root)) + "\n" for root in self.roots}


# ---- weaving --------------------------------------------------------------------
TEX_ESCAPES = {
    "\\": r"\textbackslash{}", "{": r"\{", "}": r"\}", "#": r"\#", "$": r"\$", "%": r"\%",
    "&": r"\&", "_": r"\lpus{}", "^": r"\^{}", "~": r"\textasciitilde{}",
}
QUOTES = {"'": r"\textquotesingle{}", '"': r"\textquotedbl{}"}

# Pygments occasionally lexes these as names (e.g. a chunk that starts with
# "if (...)" looks like a function declaration); they are always keywords.
CPP_KEYWORDS = set("""alignas alignof auto bool break case catch char class const constexpr continue default
delete do double else enum explicit extern false float for if inline int long namespace new noexcept nullptr
operator private protected public return short signed sizeof static static_cast reinterpret_cast struct switch
template this throw true try typedef typename union unsigned using virtual void volatile while""".split())

# C-family operators typeset the way CWEB does
OPERATORS = [("!=", r"$\ne$"), ("<=", r"$\le$"), (">=", r"$\ge$"), ("==", r"$\equiv$"), ("&&", r"$\land$"),
             ("||", r"$\lor$"), ("->", r"$\rightarrow$")]


def esc(text, quotes=True):
    out = []
    for ch in text:
        if ch in TEX_ESCAPES:
            out.append(TEX_ESCAPES[ch])
        elif quotes and ch in QUOTES:
            out.append(QUOTES[ch])
        else:
            out.append(ch)
    return "".join(out)


def esc_spaces(text):
    return "".join(r"\ " if ch == " " else ch for ch in esc(text, quotes=False))


def operator_tex(text, cfamily):
    out, i = [], 0
    while i < len(text):
        for op, tex in (OPERATORS if cfamily else []):
            if text.startswith(op, i):
                out.append(tex)
                i += len(op)
                break
        else:
            ch = text[i]
            if cfamily and ch == "!":
                out.append(r"$\neg$")
            elif ch == "-":
                out.append(r"$-$")
            else:
                out.append(esc(ch))
            i += 1
    return "".join(out)


def lexer_for(root):
    if root.endswith((".cpp", ".h", ".cu")):
        return CppLexer(), True
    if root.endswith((".txt", ".cmake")):
        return CMakeLexer(), False
    if root.endswith(".sh"):
        return BashLexer(), False
    return CppLexer(), True


BREAKABLE = re.compile(r"([/,:=])(?!\\)")


def breakable(tex):
    """Allow line breaks after / , : = in long words (paths, flags, lists)."""
    return BREAKABLE.sub(r"\1\\allowbreak{}", tex)


def render_tokens(tokens, cfamily):
    """Pygments tokens (one line, no newline) -> LaTeX."""
    out, pending_op = [], ""

    def flush():
        nonlocal pending_op
        if pending_op:
            out.append(operator_tex(pending_op, cfamily))
            pending_op = ""

    for ttype, value in tokens:
        if ttype in Operator and ttype not in Operator.Word:
            pending_op += value
            continue
        flush()
        if ttype in Whitespace or (ttype in Text and not value.strip()):
            out.append(r"\ " * len(value) if len(value) == 1 else rf"\hspace*{{{len(value)}\lpspace}}")
        elif ttype in Comment.PreprocFile:
            out.append(r"\lpstr{" + esc_spaces(value) + "}")
        elif ttype in Comment.Preproc:
            out.append(r"\lpkw{" + esc(value) + "}")
        elif ttype in Comment:
            m = re.match(r"(\s*)(//|/\*|\*/|#)?(.*?)(\*/)?$", value)
            lead, mark, body, close = m.group(1), m.group(2) or "", m.group(3), m.group(4) or ""
            out.append(r"\lpcom{" + esc(lead) + (r"\lpcommark{" + esc(mark) + "}" if mark else "") +
                       esc(body) + (r"\lpcommark{" + esc(close) + "}" if close else "") + "}")
        elif ttype in String:
            out.append(r"\lpstr{" + breakable(esc_spaces(value)) + "}")
        elif ttype in Keyword or ttype in Name.Builtin or ttype in Operator.Word:
            out.append(r"\lpkw{" + esc(value) + "}")
        elif ttype in Name and cfamily and value in CPP_KEYWORDS:
            out.append(r"\lpkw{" + esc(value) + "}")
        elif ttype in Name:
            out.append(r"\lpid{" + breakable(esc(value)) + "}")
        elif ttype in Number:
            out.append(esc(value))
        elif ttype in Punctuation:
            out.append(operator_tex(value, cfamily) if value in ("!", "-") else esc(value))
        else:
            out.append(breakable(esc(value)))
    flush()
    return "".join(out)


def split_lines(tokens):
    lines, cur = [], []
    for ttype, value in tokens:
        parts = value.split("\n")
        for i, part in enumerate(parts):
            if i:
                lines.append(cur)
                cur = []
            if part:
                cur.append((ttype, part))
    lines.append(cur)
    return lines


class Weaver:
    def __init__(self, book):
        self.book = book
        self.root_of = {}  # chunk name -> root file (for choosing the lexer)
        for root in book.roots:
            self._mark(root, root)

    def _mark(self, name, root):
        if name in self.root_of:
            return
        self.root_of[name] = root
        for c in self.book.defs[name]:
            for line in c.lines:
                m = REF_RE.match(line)
                if m:
                    self._mark(m.group(2).strip(), root)

    def ref(self, name):
        return rf"\lpref{{{self.chunk_title(name)}}}{{{self.book.number(name)}}}"

    def chunk_title(self, name):
        if name in self.book.roots:
            return r"\lpfile{" + breakable(esc(name)) + "}"
        return self.doc_text(name, refs=False)

    def inline(self, code):
        tokens = list(CppLexer().get_tokens(code))
        return r"\lpinline{" + render_tokens([t for t in tokens if t[1] != "\n"], True) + "}"

    def doc_text(self, text, refs=True):
        def code(m):
            return self.inline(m.group(1))

        text = re.sub(r"\[\[(.+?)\]\]", code, text)
        if refs:
            def chunk_ref(m):
                name = " ".join(m.group(1).split())  # a citation may span a line break
                return self.ref(name) if name in self.book.defs else m.group(0)

            text = re.sub(r"<<((?:(?!>>).)+?)>>(?!=)", chunk_ref, text, flags=re.S)
        return text

    def code_block(self, chunk):
        cfamily_root = self.root_of.get(chunk.name, chunk.name)
        lexer, cfamily = lexer_for(cfamily_root)
        placeholders = {}
        src = []
        for line in chunk.lines:
            m = REF_RE.match(line)
            if m:
                key = f"lpRef{len(placeholders)}x"
                placeholders[key] = m.group(2).strip()
                src.append(m.group(1) + key)
            else:
                src.append(line)
        tokens = list(lexer.get_tokens("\n".join(src) + "\n"))
        out = []
        for line_tokens in split_lines(tokens)[: len(src)]:
            indent = 0
            while line_tokens and (line_tokens[0][0] in Whitespace or line_tokens[0][0] in Text) \
                    and not line_tokens[0][1].strip():
                indent += len(line_tokens[0][1].expandtabs(4))
                line_tokens = line_tokens[1:]
            # a leading-whitespace token may also carry the start of the code
            if line_tokens and line_tokens[0][1][:1] == " " and line_tokens[0][1].strip():
                value = line_tokens[0][1]
                stripped = value.lstrip(" ")
                indent += len(value) - len(stripped)
                line_tokens = [(line_tokens[0][0], stripped)] + line_tokens[1:]
            body = "".join(v for _, v in line_tokens).strip()
            if body in placeholders:
                out.append(rf"\lpline{{{indent}}}{{{self.ref(placeholders[body])}}}")
            elif not line_tokens:
                out.append(r"\lpblank")
            else:
                out.append(rf"\lpline{{{indent}}}{{{render_tokens(line_tokens, cfamily)}}}")
        return "\n".join(out)

    def section_list(self, numbers):
        numbers = sorted(set(numbers))
        if len(numbers) == 1:
            return rf"section~\lpsecref{{{numbers[0]}}}"
        return "sections~" + ", ".join(rf"\lpsecref{{{n}}}" for n in numbers[:-1]) + \
            rf" and~\lpsecref{{{numbers[-1]}}}"

    def weave(self):
        b = self.book
        out = [(HERE / "preamble.tex").read_text()]
        for s in b.sections:
            title = self.doc_text(s.title, refs=False)
            out.append(rf"\lpsection{{{s.number}}}{{{s.level}}}{{{title}}}")
            out.append(self.doc_text("\n".join(s.doc)).strip() or r"\relax")
            for c in s.chunks:
                first = b.defs[c.name][0] is c
                notes = []
                if c.name in b.roots:
                    notes.append(r"This code is written to file \lpfile{" + esc(c.name) + "}.")
                elif first:
                    notes.append("This code is used in " + self.section_list(b.uses[c.name]) + ".")
                if first and len(b.defs[c.name]) > 1:
                    notes.append("See also " + self.section_list(
                        [x.section.number for x in b.defs[c.name][1:]]) + ".")
                out.append(rf"\begin{{lpchunk}}{{{self.ref(c.name)}}}{{{'' if first else '+'}}}")
                out.append(self.code_block(c))
                out.append(r"\end{lpchunk}")
                if notes:
                    out.append(r"\lpnotes{" + " ".join(notes) + "}")
        # index of chunk names, as in Knuth's "Names of the sections"
        out.append(r"\lpindexheading")
        for name in sorted(b.defs, key=lambda n: (n not in b.roots, n.lower())):
            defs = [c.section.number for c in b.defs[name]]
            used = b.uses[name]
            line = self.ref(name)
            if len(defs) > 1:
                line += r"\quad Defined in " + self.section_list(defs)[len("sections~"):] + "."
            line += r"\quad " + ("Used in " + self.section_list(used) + "." if used else "Root (written to a file).")
            out.append(r"\lpindexentry{" + line + "}")
        out.append(r"\end{document}")
        return "\n".join(out) + "\n"


# ---- command line -----------------------------------------------------------------
def main(argv):
    files = sorted((HERE / "src").glob("*.nw"))
    only = [n.strip() for n in os.environ.get("LP_ONLY", "").split(",") if n.strip()]
    if only:  # work on a subset of the book, e.g. LP_ONLY=30-scene.nw
        files = [f for f in files if f.name in only]
    if not files:
        raise LpError("no src/*.nw files")
    book = Book(files)
    cmd = argv[1] if len(argv) > 1 else "check"
    if cmd == "tangle":
        target = Path(argv[2])
        for path, text in book.tangle().items():
            dest = target / path
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text(text)
            print("wrote", dest)
    elif cmd == "check":
        tangled = book.tangle()
        ok = True
        for path in COVERED:
            if path not in tangled:
                print(f"MISSING  {path}: no root chunk")
                ok = False
                continue
            real = (REPO / path).read_text()
            if tangled[path] != real:
                a, b = tangled[path].split("\n"), real.split("\n")
                n = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
                print(f"DIFFERS  {path}: first difference at line {n + 1}")
                print(f"   book: {a[n] if n < len(a) else '<end>'!r}")
                print(f"   file: {b[n] if n < len(b) else '<end>'!r}")
                ok = False
            else:
                print(f"ok       {path}")
        for path in tangled:
            if path not in COVERED:
                print(f"EXTRA    {path}: root chunk that is not a covered file")
                ok = False
        print(f"{len(book.sections)} sections, {len(book.defs)} chunks")
        return 0 if ok else 1
    elif cmd == "weave":
        Path(argv[2]).write_text(Weaver(book).weave())
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except LpError as e:
        print("error:", e, file=sys.stderr)
        sys.exit(1)
