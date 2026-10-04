#!/usr/bin/env python3
"""Build-list lint of the hi-res fork (docs/hires/DESIGN.md, section 6.6).

Every C/C++ source and header that a build description of the tree lists (the
*_SOURCES variables of every Makefile.am, and the objects and headers named in
Makefile.common) must also be listed in

  msvcstuff/vs2019/Exult.vcxproj           (project "vcxproj")
  msvcstuff/vs2019/Exult.vcxproj.filters   (project "filters")
  ios/Exult.xcodeproj/project.pbxproj      (project "xcode")

unless an entry of the allowlist (build_lists_allow.txt next to this script)
covers it. Neither project can be built on Linux, and Makefile.mingw does not
read the vcxproj, so this lint is their only local check: a file added to the
autotools build without being added to the projects fails it.

Companions (build_lists_companions.txt next to this script): pairs of files
that must be listed together, such as a source and the new file its hooks
call. Wherever a build description lists the first file, it must list the
second one too: in every variable and every rule (prerequisites and recipe) of
each Makefile.am, Makefile.common and Makefile.mingw, under the same
conditionals or fewer; in every *.vcxproj and *.vcxproj.filters under
msvcstuff; in every target of the Xcode project and in its file references.
A source and its object count as the same file (imagewin/ibuf8.cc and
imagewin/ibuf8.o). A pair is inactive until its second file exists or is
listed somewhere.

Paths are compared relative to the repository root, ignoring case (both IDEs
run on case-insensitive file systems by default). Xcode paths are resolved
through the project's group tree. Generated files (those a makefile rule or
BUILT_SOURCES produces, such as data/exult_flx.h and gitinfo.h, and anything
git ignores) and files that do not exist are not checked. New files are checked
before they are added to git.

Usage: check_build_lists.py [--root DIR] [--allowlist FILE] [--companions FILE]
                            [--strict] [--list-missing] [--verbose]
Exit status: 0 consistent, 1 unlisted files or missing companions (or, with
--strict, allowlist or companion entries whose files are listed nowhere),
2 usage or parse error.
Python 3.8 or newer, standard library only.
"""

import argparse
import os
import posixpath
import re
import subprocess
import sys
import xml.etree.ElementTree as ElementTree

PROJECTS = ("vcxproj", "filters", "xcode")
PROJECT_FILES = {
    "vcxproj": "msvcstuff/vs2019/Exult.vcxproj",
    "filters": "msvcstuff/vs2019/Exult.vcxproj.filters",
    "xcode": "ios/Exult.xcodeproj/project.pbxproj",
}
SOURCE_EXTS = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".m", ".mm")
OBJECT_SOURCE_EXTS = (".cc", ".cpp", ".c", ".cxx", ".mm", ".m")
# Compiled sources and objects: their companion keys drop the extension.
COMPILED_EXTS = OBJECT_SOURCE_EXTS + (".o", ".obj", ".lo")
MAKEFILE_NAMES = ("Makefile.am", "Makefile.common", "Makefile.mingw")
CONDITIONAL_START = re.compile(r"^\s*(?:if|ifeq|ifneq|ifdef|ifndef)\b")


class LintError(Exception):
    pass


def norm(path):
    """Repository-relative POSIX path, without "./" and ".." where possible."""
    return posixpath.normpath(path.replace("\\", "/"))


def unit_key(path):
    """Key of a listed file for the companion rule: lower case, and without the
    extension for compiled sources and objects (imagewin/ibuf8.cc and imagewin/ibuf8.o
    are both imagewin/ibuf8); headers and other files keep theirs."""
    path = norm(path).lower()
    stem, ext = posixpath.splitext(path)
    return stem if ext in COMPILED_EXTS else path


def walk_tree(root):
    """(directory relative to root, file names) for the tree, without hidden and
    autoconf cache directories, in a stable order."""
    for directory, dirs, files in os.walk(root):
        dirs[:] = sorted(d for d in dirs if not d.startswith(".") and d != "autom4te.cache")
        rel = norm(os.path.relpath(directory, root))
        yield ("" if rel == "." else rel), sorted(files)


# --------------------------------------------------------------------------
# Makefiles


def logical_lines(text):
    """Lines with backslash continuations joined and comments removed."""
    lines = []
    current = ""
    for raw in text.splitlines():
        if raw.endswith("\\"):
            current += raw[:-1] + " "
            continue
        current += raw
        lines.append(current)
        current = ""
    if current:
        lines.append(current)
    result = []
    for line in lines:
        if line.startswith("\t"):
            result.append(line)    # A recipe line: kept as is.
            continue
        hash_pos = line.find("#")
        if hash_pos >= 0:
            line = line[:hash_pos]
        result.append(line.rstrip())
    return result


ASSIGNMENT = re.compile(r"^\s*([A-Za-z0-9_.]+)\s*(\+=|:=|::=|\?=|=)\s*(.*)$")
VARIABLE_REF = re.compile(r"\$[({]([A-Za-z0-9_.]+)[)}]")


def parse_variables(lines):
    """Variable name -> list of value strings; all conditional branches count."""
    variables = {}
    for line in lines:
        if line.startswith("\t"):
            continue
        match = ASSIGNMENT.match(line)
        if match:
            variables.setdefault(match.group(1), []).append(match.group(3))
    return variables


def expand(value, variables, seen=()):
    """Expands $(VAR) references to variables defined in the same file; other
    references (configure substitutions, $(srcdir), ...) stay as they are."""

    def replace(match):
        name = match.group(1)
        if name in seen:
            return " "
        if name not in variables:
            return match.group(0)
        return " " + " ".join(expand(v, variables, seen + (name,)) for v in variables[name]) + " "

    return VARIABLE_REF.sub(replace, value)


def strip_dir_prefix(token):
    for prefix in ("$(srcdir)/", "$(top_srcdir)/", "${srcdir}/", "${top_srcdir}/", "$(SRC)/", "./"):
        if token.startswith(prefix):
            return token[len(prefix):], prefix
    return token, ""


def rule_targets(lines, variables, directory):
    """Files that the rules of a makefile produce (expanded targets), relative to the root."""
    produced = set()
    for line in lines:
        if line.startswith("\t") or not line.strip() or ASSIGNMENT.match(line) or ":" not in line:
            continue
        targets, _, prerequisites = line.partition(":")
        if prerequisites.startswith("=") or "=" in prerequisites:
            continue    # A target-specific variable.
        for token in expand(targets, variables).split():
            if "$" in token or "%" in token or token.startswith("."):
                continue
            token, prefix = strip_dir_prefix(token)
            base = "" if prefix.startswith(("$(top_srcdir)", "${top_srcdir}")) else directory
            produced.add(norm(posixpath.join(base, token)))
    return produced


def makefile_am_sources(root, rel_path):
    """({file: origin} of the *_SOURCES variables, set of generated files)."""
    with open(os.path.join(root, rel_path), encoding="utf-8", errors="replace") as handle:
        lines = logical_lines(handle.read())
    variables = parse_variables(lines)
    directory = posixpath.dirname(rel_path)

    def files_of(value):
        for token in expand(value, variables).split():
            if "$" in token or not token.endswith(SOURCE_EXTS):
                continue
            token, prefix = strip_dir_prefix(token)
            base = "" if prefix.startswith(("$(top_srcdir)", "${top_srcdir}")) else directory
            yield norm(posixpath.join(base, token))

    found = {}
    generated = rule_targets(lines, variables, directory)
    for name, values in variables.items():
        if name == "BUILT_SOURCES" or name.startswith("nodist_"):
            for value in values:
                generated.update(files_of(value))
        elif name.endswith("_SOURCES"):
            for value in values:
                for path in files_of(value):
                    found.setdefault(path, rel_path)
    return found, generated


def makefile_common_sources(root, rel_path, verbose):
    """({file: origin} of the objects (mapped to their sources) and headers it names,
    set of generated files)."""
    with open(os.path.join(root, rel_path), encoding="utf-8", errors="replace") as handle:
        lines = logical_lines(handle.read())
    variables = parse_variables(lines)
    tokens = []
    for line in lines:
        if line.startswith("\t") or not line.strip():
            continue
        match = ASSIGNMENT.match(line)
        if match:
            tokens.extend(match.group(3).split())
        elif ":" in line:
            _, _, prerequisites = line.partition(":")
            if "=" in prerequisites:
                continue    # A target-specific variable: no file names.
            tokens.extend(prerequisites.split())
    found = {}
    for token in tokens:
        if "$" in token and not token.startswith("$(SRC)/"):
            continue
        if "%" in token:
            continue
        token, _ = strip_dir_prefix(token)
        if token.endswith(".o"):
            stem = token[:-2]
            for ext in OBJECT_SOURCE_EXTS:
                if os.path.isfile(os.path.join(root, stem + ext)):
                    found.setdefault(norm(stem + ext), rel_path)
                    break
            else:
                if verbose:
                    print(f"note: {rel_path}: no source for {token} (generated?)")
        elif token.endswith(SOURCE_EXTS):
            found.setdefault(norm(token), rel_path)
    return found, rule_targets(lines, variables, "")


def build_list_files(root, verbose):
    """{file: origin} of every existing file the build descriptions list."""
    found = {}
    generated = set()
    makefiles = []
    for directory, files in walk_tree(root):
        if "Makefile.am" in files:
            makefiles.append(makefile_am_sources(root, posixpath.join(directory, "Makefile.am")))
    makefiles.append(makefile_common_sources(root, "Makefile.common", verbose))
    for sources, produced in makefiles:
        for path, origin in sources.items():
            found.setdefault(path, origin)
        generated |= produced
    ignored = git_ignored(root, sorted(found))
    listed = {}
    for path, origin in found.items():
        if path in generated:
            if verbose:
                print(f"note: {origin}: {path} is made by a build rule (generated), not checked")
        elif path in ignored:
            if verbose:
                print(f"note: {origin}: {path} is ignored by git (generated), not checked")
        elif os.path.isfile(os.path.join(root, path)):
            listed[path] = origin
        elif verbose:
            print(f"note: {origin}: {path} does not exist (generated or stale), not checked")
    return listed


def makefile_lists(root, rel_path):
    """The lists of one makefile for the companion rule, {name: [(conditionals, keys)]}.
    Each assignment of a variable (under the conditionals around it) and each rule
    (prerequisites and recipe lines) is an entry; values are not expanded, so a file
    belongs to the variable or rule that names it. Paths are relative to the
    makefile's directory, as automake reads them."""
    with open(os.path.join(root, rel_path), encoding="utf-8", errors="replace") as handle:
        lines = logical_lines(handle.read())
    directory = posixpath.dirname(rel_path)

    def keys(text):
        result = set()
        for token in text.split():
            token, prefix = strip_dir_prefix(token)
            if "$" in token or "%" in token:
                continue
            base = "" if prefix.startswith(("$(top_srcdir)", "${top_srcdir}")) else directory
            result.add(unit_key(posixpath.join(base, token)))
        return result

    lists = {}
    conditionals = []    # [text, branch] of each open if/ifeq/...; "else" moves to the next branch.
    rule = None    # The entry whose recipe lines follow.
    in_define = False
    for line in lines:
        word = line.split(None, 1)[0] if line.strip() else ""
        if in_define:
            in_define = word != "endef"
            continue
        if line.startswith("\t"):
            if rule is not None:
                rule[1].update(keys(line))
            continue
        if not word:
            continue    # Blank lines and comments do not end a recipe.
        rule = None
        if word == "define":
            in_define = True
        elif CONDITIONAL_START.match(line):
            conditionals.append([" ".join(line.split()), 0])
        elif word == "else":
            if conditionals:
                conditionals[-1][1] += 1
        elif word == "endif":
            if conditionals:
                conditionals.pop()
        else:
            state = tuple((text, branch) for text, branch in conditionals)
            match = ASSIGNMENT.match(line)
            if match:
                lists.setdefault(f"{rel_path}: {match.group(1)}", []).append((state, keys(match.group(3))))
            elif ":" in line:
                targets, _, prerequisites = line.partition(":")
                if "=" not in prerequisites:    # Otherwise a target-specific variable.
                    rule = (state, keys(prerequisites.lstrip(":")))
                    lists.setdefault(f"{rel_path}: rule {' '.join(targets.split())}", []).append(rule)
    return lists


def git_ignored(root, paths):
    """The paths git ignores (generated files such as data/exult_flx.h); empty without git."""
    try:
        result = subprocess.run(
            ["git", "-C", root, "check-ignore", "--stdin"],
            input="\n".join(paths) + "\n",
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError:
        return set()
    if result.returncode not in (0, 1):    # 1: nothing ignored; 128: not a git tree.
        return set()
    return {norm(line) for line in result.stdout.splitlines() if line}


# --------------------------------------------------------------------------
# Visual Studio


def vcxproj_files(root, rel_path):
    try:
        tree = ElementTree.parse(os.path.join(root, rel_path))
    except (OSError, ElementTree.ParseError) as error:
        raise LintError(f"{rel_path}: {error}") from error
    directory = posixpath.dirname(rel_path)
    files = set()
    for element in tree.iter():
        include = element.get("Include")
        if include and element.tag.split("}")[-1] != "ProjectConfiguration":
            files.add(norm(posixpath.join(directory, include.replace("\\", "/"))).lower())
    return files


# --------------------------------------------------------------------------
# Xcode (an OpenStep-style property list)

PLIST_TOKEN = re.compile(r'\s*(?:(//[^\n]*)|(/\*.*?\*/)|("(?:[^"\\]|\\.)*")|([{}()=;,])|([^\s{}()=;,"]+))', re.S)


def plist_tokens(text):
    pos = 0
    while True:
        match = PLIST_TOKEN.match(text, pos)
        if not match or match.end() == pos:
            if text[pos:].strip():
                raise LintError(f"pbxproj: cannot parse at offset {pos}")
            return
        pos = match.end()
        if match.group(1) or match.group(2):
            continue
        if match.group(3) is not None:
            yield ("string", re.sub(r"\\(.)", r"\1", match.group(3)[1:-1]))
        elif match.group(4):
            yield ("punct", match.group(4))
        else:
            yield ("string", match.group(5))


def parse_plist(text):
    tokens = list(plist_tokens(text))
    pos = 0

    def value():
        nonlocal pos
        kind, token = tokens[pos]
        pos += 1
        if kind == "string":
            return token
        if token == "{":
            result = {}
            while tokens[pos] != ("punct", "}"):
                key = value()
                if tokens[pos] != ("punct", "="):
                    raise LintError(f"pbxproj: '=' expected after {key!r}")
                pos += 1
                result[key] = value()
                if tokens[pos] == ("punct", ";"):
                    pos += 1
            pos += 1
            return result
        if token == "(":
            result = []
            while tokens[pos] != ("punct", ")"):
                result.append(value())
                if tokens[pos] == ("punct", ","):
                    pos += 1
            pos += 1
            return result
        raise LintError(f"pbxproj: unexpected {token!r}")

    try:
        return value()
    except IndexError as error:
        raise LintError("pbxproj: unexpected end of file") from error


def xcode_files(root, rel_path):
    return xcode_project(root, rel_path)[0]


def xcode_project(root, rel_path):
    """(set of the project's file paths, {target name: set of the paths in its build phases})."""
    try:
        with open(os.path.join(root, rel_path), encoding="utf-8-sig") as handle:    # The file starts with a BOM.
            plist = parse_plist(handle.read())
    except OSError as error:
        raise LintError(f"{rel_path}: {error}") from error
    objects = plist.get("objects", {})
    project_dir = posixpath.dirname(posixpath.dirname(rel_path))    # The directory of the .xcodeproj.
    project = objects.get(plist.get("rootObject", ""), {})
    base = norm(posixpath.join(project_dir, project.get("projectDirPath", "") or ""))

    def resolve(obj, parent):
        tree = obj.get("sourceTree", "<group>")
        path = obj.get("path")
        if tree == "<group>":
            return norm(posixpath.join(parent, path)) if path else parent
        if tree == "SOURCE_ROOT":
            return norm(posixpath.join(base, path)) if path else base
        return None    # SDKROOT, BUILT_PRODUCTS_DIR, <absolute>, ...: not repository files.

    path_of = {}    # File reference id -> repository path (lower case).

    def walk(object_id, parent):
        obj = objects.get(object_id, {})
        path = resolve(obj, parent)
        if obj.get("isa") in ("PBXGroup", "PBXVariantGroup", "XCVersionGroup"):
            for child in obj.get("children", []):
                walk(child, path if path is not None else parent)
        elif obj.get("isa") == "PBXFileReference":
            path_of[object_id] = path.lower() if path is not None else None

    walk(project.get("mainGroup", ""), base)
    # File references outside the group tree: relative to the project directory.
    for object_id, obj in objects.items():
        if obj.get("isa") == "PBXFileReference" and object_id not in path_of:
            path = resolve(obj, base)
            path_of[object_id] = path.lower() if path is not None else None
    files = {path for path in path_of.values() if path is not None}

    targets = {}
    for obj in objects.values():
        if obj.get("isa") != "PBXNativeTarget":
            continue
        members = set()
        for phase_id in obj.get("buildPhases", []):
            for build_file_id in objects.get(phase_id, {}).get("files", []):
                path = path_of.get(objects.get(build_file_id, {}).get("fileRef"))
                if path is not None:
                    members.add(path)
        targets[obj.get("name", "?")] = members
    return files, targets


def vcxproj_lists(root):
    """{project file: [((), keys)]} of every *.vcxproj and *.vcxproj.filters under msvcstuff."""
    lists = {}
    for directory, files in walk_tree(os.path.join(root, "msvcstuff")):
        for name in files:
            if name.endswith((".vcxproj", ".vcxproj.filters")):
                rel = norm(posixpath.join("msvcstuff", directory, name))
                lists[rel] = [((), {unit_key(path) for path in vcxproj_files(root, rel)})]
    return lists


def companion_lists(root, xcode):
    """{list name: [(conditionals, keys)]} of every build description, for the companion
    rule; xcode is the result of xcode_project() for PROJECT_FILES["xcode"]."""
    lists = {}
    for directory, files in walk_tree(root):
        for name in files:
            if name in MAKEFILE_NAMES:
                lists.update(makefile_lists(root, posixpath.join(directory, name)))
    lists.update(vcxproj_lists(root))
    files, targets = xcode
    lists[f"{PROJECT_FILES['xcode']}: file references"] = [((), {unit_key(path) for path in files})]
    for name, members in sorted(targets.items()):
        lists[f"{PROJECT_FILES['xcode']}: target {name}"] = [((), {unit_key(path) for path in members})]
    return lists


# --------------------------------------------------------------------------
# Allowlist


def glob_to_regex(pattern):
    """'**' matches across directories, '*' and '?' within one."""
    out = []
    i = 0
    while i < len(pattern):
        if pattern.startswith("**", i):
            out.append(".*")
            i += 2
        elif pattern[i] == "*":
            out.append("[^/]*")
            i += 1
        elif pattern[i] == "?":
            out.append("[^/]")
            i += 1
        else:
            out.append(re.escape(pattern[i]))
            i += 1
    return re.compile("".join(out) + r"\Z")


def read_allowlist(path):
    entries = []
    with open(path, encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            text = line.split("#", 1)[0].strip()
            if not text:
                continue
            fields = text.split()
            if len(fields) != 2:
                raise LintError(f"{path}:{number}: expected '<glob> <projects>'")
            projects = set(PROJECTS) if fields[1] == "all" else set(fields[1].split(","))
            unknown = projects - set(PROJECTS)
            if unknown:
                raise LintError(f"{path}:{number}: unknown project(s) {', '.join(sorted(unknown))}")
            entries.append({"line": number, "glob": fields[0], "regex": glob_to_regex(fields[0]), "projects": projects, "used": False})
    return entries


# --------------------------------------------------------------------------
# Companions


def read_companions(path):
    entries = []
    with open(path, encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            text = line.split("#", 1)[0].strip()
            if not text:
                continue
            fields = text.split()
            if len(fields) != 2:
                raise LintError(f"{path}:{number}: expected '<first> <second>'")
            entries.append({"line": number, "first": norm(fields[0]), "second": norm(fields[1])})
    return entries


def check_companions(root, companions, lists, verbose):
    """(failures, entries whose first file is listed nowhere, inactive entries)."""
    failures = []
    stale = []
    inactive = []
    for entry in companions:
        first = unit_key(entry["first"])
        second = unit_key(entry["second"])
        holders = [name for name in sorted(lists) if any(first in keys for _, keys in lists[name])]
        if not holders:
            stale.append(entry)
            continue
        if not any(second in keys for entries in lists.values() for _, keys in entries) and not os.path.exists(
            os.path.join(root, entry["second"])
        ):
            inactive.append(entry)
            if verbose:
                print(f"note: companion {entry['second']} of {entry['first']} does not exist and is listed nowhere: inactive")
            continue
        if verbose:
            print(f"note: companion {entry['second']} of {entry['first']}: checked in {len(holders)} lists")
        for name in holders:
            entries = lists[name]
            for state, keys in entries:
                if first not in keys:
                    continue
                # The second file must be in this list under the same conditionals or fewer.
                if any(second in other and state[: len(other_state)] == other_state for other_state, other in entries):
                    continue
                where = name
                if state:
                    where += " (inside " + ", ".join(text if branch == 0 else f"else of {text}" for text, branch in state) + ")"
                message = f"{entry['second']}: must be listed with {entry['first']}, missing from {where}"
                if message not in failures:
                    failures.append(message)
    return failures, stale, inactive


# --------------------------------------------------------------------------


def main(argv):
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", default=os.path.dirname(os.path.dirname(here)), help="repository root (default: ../.. of this script)")
    parser.add_argument("--allowlist", default=os.path.join(here, "build_lists_allow.txt"))
    parser.add_argument("--companions", default=os.path.join(here, "build_lists_companions.txt"))
    parser.add_argument("--strict", action="store_true", help="also fail on allowlist and companion entries whose files are listed nowhere")
    parser.add_argument("--list-missing", action="store_true", help="print every unlisted file, ignoring the allowlist")
    parser.add_argument("--verbose", "-v", action="store_true")
    args = parser.parse_args(argv)

    try:
        listed = build_list_files(args.root, args.verbose)
        xcode = xcode_project(args.root, PROJECT_FILES["xcode"])
        project_files = {
            "vcxproj": vcxproj_files(args.root, PROJECT_FILES["vcxproj"]),
            "filters": vcxproj_files(args.root, PROJECT_FILES["filters"]),
            "xcode": xcode[0],
        }
        allowlist = [] if args.list_missing else read_allowlist(args.allowlist)
        companions = [] if args.list_missing else read_companions(args.companions)
        lists = companion_lists(args.root, xcode) if companions else {}
    except (LintError, OSError) as error:
        print(f"check_build_lists: {error}", file=sys.stderr)
        return 2

    missing = {}    # path -> set of projects
    for path in sorted(listed):
        absent = {name for name in PROJECTS if path.lower() not in project_files[name]}
        if absent:
            missing[path] = absent

    if args.list_missing:
        for path, absent in missing.items():
            names = "all" if absent == set(PROJECTS) else ",".join(name for name in PROJECTS if name in absent)
            print(f"{path} {names}    # {listed[path]}")
        return 0

    failures = []
    for path, absent in missing.items():
        for name in sorted(absent, key=PROJECTS.index):
            entry = next((e for e in allowlist if name in e["projects"] and e["regex"].match(path)), None)
            if entry:
                entry["used"] = True
            else:
                failures.append(f"{path}: listed in {listed[path]}, missing from {PROJECT_FILES[name]}")

    stale = [e for e in allowlist if not e["used"]]
    for entry in stale:
        print(f"{'error' if args.strict else 'warning'}: {args.allowlist}:{entry['line']}: '{entry['glob']}' matches no unlisted file", file=sys.stderr)
    companion_failures, stale_companions, inactive = check_companions(args.root, companions, lists, args.verbose)
    for entry in stale_companions:
        print(
            f"{'error' if args.strict else 'warning'}: {args.companions}:{entry['line']}: {entry['first']} is listed nowhere",
            file=sys.stderr,
        )
    for failure in failures + companion_failures:
        print(failure)
    print(
        f"check_build_lists: {len(listed)} files in the build lists, {len(missing)} not in every project, "
        f"{len(failures)} not allowed, {len(stale)} unused allowlist entries; "
        f"{len(companions)} companion pairs ({len(inactive)} inactive), {len(companion_failures)} missing companions"
    )
    if failures or companion_failures or (args.strict and (stale or stale_companions)):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
