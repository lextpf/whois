"""Pre-pass to clean doxide-generated markdown before mkdocs build.

Cleans doxide-generated markdown for MkDocs Material compatibility:
- Removes @author lines
- Strips @brief tags
- Fixes admonition indentation (1-space to 4-space)
- Adds Material icons to page titles and section headers
- Trims function summary tables to first-sentence briefs
- Flattens namespace definition lists into single-line bullets
- Injects version from src/Version.h into home page subtitle

Only touches files with 'generator: doxide' frontmatter.

Usage:
    python scripts/clean_docs.py          # defaults to docs/
    python scripts/clean_docs.py path/    # custom docs directory
"""

import re
import sys
from pathlib import Path


def is_doxide_generated(text: str) -> bool:
    return "generator: doxide" in text[:200]


def fix_admonition_indent(text: str) -> str:
    """Fix doxide's 1-space admonition indent to 4-space for MkDocs Material."""
    lines = text.split("\n")
    result = []
    in_admonition = False

    for line in lines:
        if re.match(r"^!!! \w+", line):
            in_admonition = True
            result.append(line)
            continue

        if in_admonition:
            # Body line with 1-space indent
            m = re.match(r"^ (\S.*)", line)
            if m:
                result.append("    " + m.group(1))
                continue
            # Continuation with deeper indent
            if line.startswith("  "):
                result.append("    " + line.lstrip())
                continue
            # Blank or unindented line ends the admonition
            in_admonition = False

        result.append(line)

    return "\n".join(result)


PAGE_TITLE_ICONS = {
    "Core":           ":material-cube-outline:",
    "Rendering":      ":material-palette:",
    "Configuration":  ":material-cog-outline:",
    "Hooks":          ":material-hook:",
    "Utilities":      ":material-toolbox-outline:",
}


SECTION_ICONS = {
    "Types":               ":material-shape-outline:",
    "Functions":           ":material-function:",
    "Variables":           ":material-variable:",
    "Macros":              ":material-pound:",
    "Operators":           ":material-math-compass:",
    "Type Aliases":        ":material-link-variant:",
    "Type Details":        ":material-shape-outline:",
    "Type Alias Details":  ":material-link-variant:",
    "Function Details":    ":material-function:",
    "Variable Details":    ":material-variable:",
    "Macro Details":       ":material-pound:",
    "Operator Details":    ":material-math-compass:",
}


def add_page_title_icons(text: str) -> str:
    """Prepend Material icons to doxide-generated H1 page titles."""
    for title, icon in PAGE_TITLE_ICONS.items():
        text = re.sub(rf"^# {re.escape(title)}$", f"# {icon} {title}", text, count=1, flags=re.MULTILINE)
    return text


def add_section_icons(text: str) -> str:
    """Prepend Material icons to doxide-generated section headers."""
    for title, icon in SECTION_ICONS.items():
        text = text.replace(f"## {title}", f"## {icon} {title}")
    return text


def trim_function_table_descriptions(text: str) -> str:
    """Keep only a concise brief sentence in doxide function summary tables.

    Doxide occasionally emits full function details into the `Functions` table
    description column. This pass trims each function row to the first sentence
    so detailed content remains only under `Function Details`.
    """
    lines = text.split("\n")
    out = []
    in_functions_table = False

    for line in lines:
        stripped = line.strip()

        if stripped in {"## Functions", "## :material-function: Functions"}:
            in_functions_table = True
            out.append(line)
            continue

        # End table context on next section header.
        if in_functions_table and stripped.startswith("## "):
            in_functions_table = False
            out.append(line)
            continue

        if in_functions_table and stripped.startswith("| [") and stripped.endswith("|"):
            parts = [p.strip() for p in stripped.strip("|").split("|", 1)]
            if len(parts) == 2:
                name_col, desc_col = parts
                desc_col = re.sub(r"\s+", " ", desc_col).strip()
                # Keep only first sentence in summary table.
                m = re.match(r"^(.*?\.)\s+.*$", desc_col)
                brief = m.group(1) if m else desc_col
                out.append(f"| {name_col} | {brief} |")
                continue

        out.append(line)

    return "\n".join(out)


def flatten_namespace_lists(text: str) -> str:
    """Flatten doxide namespace definition lists into single-line bullets.

    Home page namespace entries are emitted as definition lists:
        :material-package: [Name](...)
        :    Description
    which renders description on the next line. Convert these to:
        - :material-package: [Name](...) - Description
    """
    lines = text.split("\n")
    out = []
    i = 0

    while i < len(lines):
        line = lines[i]
        term = line.strip()
        if term.startswith(":material-package:") or term.startswith(":material-format-section:"):
            desc = ""
            if i + 1 < len(lines):
                m = re.match(r"^:\s+(.*)$", lines[i + 1])
                if m:
                    desc = m.group(1).strip()
                    i += 1

            if desc:
                out.append(f"- {term} - {desc}")
            else:
                out.append(f"- {term}")

            i += 1
            if i < len(lines) and lines[i].strip() == "":
                i += 1
            continue

        out.append(line)
        i += 1

    return "\n".join(out)


def parse_version(repo_root: Path) -> str:
    """Read version components from src/Version.h and return 'MAJOR.MINOR.PATCH'."""
    version_h = repo_root / "src" / "Version.h"
    if not version_h.exists():
        return ""
    content = version_h.read_text(encoding="utf-8")
    parts = {}
    for key in ("MAJOR", "MINOR", "PATCH"):
        m = re.search(rf"#define\s+WHOIS_VERSION_{key}\s+(\d+)", content)
        if m:
            parts[key] = m.group(1)
    if len(parts) == 3:
        return f"{parts['MAJOR']}.{parts['MINOR']}.{parts['PATCH']}"
    return ""


def inject_version(text: str, version: str) -> str:
    """Add version to the home page subtitle line."""
    if not version:
        return text
    return re.sub(
        r"^(# whois)\n\n(.+)$",
        rf"\1\n\n**v{version}** | \2",
        text,
        count=1,
        flags=re.MULTILINE,
    )


def clean(text: str) -> str:
    # Remove standalone @author lines
    text = re.sub(r"^\s*@author\b.*\n?", "", text, flags=re.MULTILINE)

    # Strip @brief tag but keep the description text
    text = re.sub(r"@brief\s+", "", text)

    # Fix admonition indentation (doxide outputs 1-space, MkDocs needs 4)
    text = fix_admonition_indent(text)

    # Add icons to page titles
    text = add_page_title_icons(text)

    # Add icons to section headers
    text = add_section_icons(text)

    # Trim over-detailed function summary table entries.
    text = trim_function_table_descriptions(text)

    # Keep namespace descriptions inline on Home/namespace listings.
    text = flatten_namespace_lists(text)

    return text


def main():
    docs_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("docs")

    if not docs_dir.is_dir():
        print(f"error: {docs_dir} is not a directory", file=sys.stderr)
        sys.exit(1)

    repo_root = docs_dir.resolve().parent
    version = parse_version(repo_root)
    if version:
        print(f"  version: {version}")

    changed = 0
    for md in docs_dir.rglob("*.md"):
        original = md.read_text(encoding="utf-8")
        if not is_doxide_generated(original):
            continue

        cleaned = clean(original)

        # Inject version into home page
        if md.name == "index.md" and md.parent == docs_dir and version:
            cleaned = inject_version(cleaned, version)

        if cleaned != original:
            md.write_text(cleaned, encoding="utf-8")
            changed += 1
            print(f"  cleaned {md.relative_to(docs_dir)}")

    print(f"done: {changed} file(s) cleaned")


if __name__ == "__main__":
    main()
