"""Tests of tools/hires/check_build_lists.py on a small synthetic repository."""

import os
import subprocess
import sys
import textwrap

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(os.path.dirname(HERE), "check_build_lists.py")

MAKEFILE_AM = """\
noinst_LTLIBRARIES = libimagewin.la
libimagewin_la_SOURCES = \\
\tibuf8.cc \\
\tibuf8.h
if BUILD_EXULT
libimagewin_la_SOURCES += imagewin.cc
endif
BUILT_SOURCES = made.h
made.h: ibuf8.h
\tcp $< $@
"""

MAKEFILE_COMMON = """\
IMAGEWIN_OBJS:=\\
\timagewin/ibuf8.o \\
\timagewin/imagewin.o
GEN := data/gen_flx.h
$(GEN): imagewin/ibuf8.o
\ttouch $@
exult.o: exult.cc $(GEN)
"""


def vcxproj(files, tag="ClCompile"):
    items = "\n".join(f'    <{tag} Include="..\\..\\{f.replace("/", chr(92))}" />' for f in files)
    return textwrap.dedent(
        """\
        <?xml version="1.0" encoding="utf-8"?>
        <Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
          <ItemGroup>
            <ProjectConfiguration Include="Debug|x64" />
          </ItemGroup>
          <ItemGroup>
        """
    ) + items + "\n  </ItemGroup>\n</Project>\n"


def pbxproj(files, targets=None):
    """A project whose group tree places each file under a group named after its directory.
    targets: {name: files} adds a native target whose sources phase builds those files."""
    objects = []
    groups = {}
    for i, path in enumerate(files):
        directory, name = os.path.split(path)
        groups.setdefault(directory, []).append(f"F{i:03d}")
        objects.append(f'\t\tF{i:03d} /* {name} */ = {{isa = PBXFileReference; path = "{name}"; sourceTree = "<group>"; }};')
    children = []
    for j, (directory, refs) in enumerate(sorted(groups.items())):
        gid = f"G{j:03d}"
        children.append(gid)
        path = f'path = "../{directory}"; ' if directory else 'path = ..; '
        objects.append(f'\t\t{gid} = {{isa = PBXGroup; children = ({", ".join(refs)}, ); {path}sourceTree = "<group>"; }};')
    objects.append(f'\t\tMAIN = {{isa = PBXGroup; children = ({", ".join(children)}, ); sourceTree = "<group>"; }};')
    for k, (target, members) in enumerate(sorted((targets or {}).items())):
        build_files = []
        for path in members:
            bid = f"B{k:02d}{files.index(path):03d}"
            build_files.append(bid)
            objects.append(f"\t\t{bid} /* {path} in Sources */ = {{isa = PBXBuildFile; fileRef = F{files.index(path):03d}; }};")
        objects.append(f'\t\tS{k:03d} /* Sources */ = {{isa = PBXSourcesBuildPhase; files = ({", ".join(build_files)}, ); }};')
        objects.append(f'\t\tT{k:03d} /* {target} */ = {{isa = PBXNativeTarget; buildPhases = (S{k:03d}, ); name = "{target}"; }};')
    objects.append('\t\tROOT /* Project object */ = {isa = PBXProject; mainGroup = MAIN; projectDirPath = ""; };')
    return "﻿// !$*UTF8*$!\n{\n\tarchiveVersion = 1;\n\tobjects = {\n" + "\n".join(objects) + "\n\t};\n\trootObject = ROOT;\n}\n"


class Repo:
    """A minimal tree: one Makefile.am, Makefile.common and the three projects."""

    FILES = ["imagewin/ibuf8.cc", "imagewin/ibuf8.h", "imagewin/imagewin.cc", "exult.cc"]

    def __init__(self, root):
        self.root = root
        for path in self.FILES:
            (root / path).parent.mkdir(parents=True, exist_ok=True)
            (root / path).write_text("// test\n")
        (root / "imagewin" / "Makefile.am").write_text(MAKEFILE_AM)
        (root / "Makefile.common").write_text(MAKEFILE_COMMON)
        (root / "msvcstuff" / "vs2019").mkdir(parents=True)
        (root / "ios" / "Exult.xcodeproj").mkdir(parents=True)
        (root / "allow.txt").write_text("# empty\n")
        (root / "companions.txt").write_text("# empty\n")
        self.write_projects()

    def __truediv__(self, path):
        return self.root / path

    def write_projects(self, vc=None, filters=None, xcode=None, targets=None):
        root = self.root
        (root / "msvcstuff" / "vs2019" / "Exult.vcxproj").write_text(vcxproj(vc or self.FILES))
        (root / "msvcstuff" / "vs2019" / "Exult.vcxproj.filters").write_text(vcxproj(filters or self.FILES, "ClInclude"))
        (root / "ios" / "Exult.xcodeproj" / "project.pbxproj").write_text(pbxproj(xcode or self.FILES, targets), encoding="utf-8")


@pytest.fixture
def repo(tmp_path):
    return Repo(tmp_path / "repo")


def run(repo, *args):
    return subprocess.run(
        [
            sys.executable,
            SCRIPT,
            "--root",
            str(repo.root),
            "--allowlist",
            str(repo / "allow.txt"),
            "--companions",
            str(repo / "companions.txt"),
            *args,
        ],
        capture_output=True,
        text=True,
        check=False,
    )


def test_consistent_tree_passes(repo):
    result = run(repo, "--strict")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "4 files in the build lists, 0 not in every project" in result.stdout


def test_file_missing_from_a_project_fails(repo):
    repo.write_projects(xcode=["imagewin/ibuf8.cc", "imagewin/ibuf8.h", "exult.cc"])
    result = run(repo)
    assert result.returncode == 1
    assert "imagewin/imagewin.cc: listed in imagewin/Makefile.am, missing from ios/Exult.xcodeproj/project.pbxproj" in result.stdout
    assert "Exult.vcxproj" not in result.stdout


def test_new_file_must_be_registered(repo):
    (repo / "imagewin" / "ibuf8_scaled.cc").write_text("// new\n")
    with open(repo / "imagewin" / "Makefile.am", "a") as makefile:
        makefile.write("libimagewin_la_SOURCES += ibuf8_scaled.cc\n")
    result = run(repo)
    assert result.returncode == 1
    assert result.stdout.count("imagewin/ibuf8_scaled.cc: listed in imagewin/Makefile.am") == 3


def test_allowlist_covers_and_strict_reports_unused_entries(repo):
    repo.write_projects(vc=["imagewin/ibuf8.cc", "imagewin/ibuf8.h", "exult.cc"])
    (repo / "allow.txt").write_text("imagewin/imagewin.*  vcxproj    # reason\ntools/**  all\n")
    assert run(repo).returncode == 0
    strict = run(repo, "--strict")
    assert strict.returncode == 1
    assert "'tools/**' matches no unlisted file" in strict.stderr
    # The entry for vcxproj does not cover the filters project.
    repo.write_projects(filters=["imagewin/ibuf8.cc", "imagewin/ibuf8.h", "exult.cc"])
    assert run(repo).returncode == 1


def test_generated_and_absent_files_are_not_checked(repo):
    # made.h (BUILT_SOURCES and a rule target) and data/gen_flx.h (a rule target in
    # Makefile.common) exist on disk but are not sources.
    (repo / "imagewin" / "made.h").write_text("// generated\n")
    (repo / "data").mkdir()
    (repo / "data" / "gen_flx.h").write_text("// generated\n")
    with open(repo / "imagewin" / "Makefile.am", "a") as makefile:
        makefile.write("libimagewin_la_SOURCES += made.h gone.cc\n")
    result = run(repo, "--strict", "--verbose")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "imagewin/made.h is made by a build rule" in result.stdout
    assert "data/gen_flx.h is made by a build rule" in result.stdout
    assert "imagewin/gone.cc does not exist" in result.stdout


def test_list_missing_prints_allowlist_lines(repo):
    repo.write_projects(vc=["exult.cc"], filters=["exult.cc"], xcode=["exult.cc"])
    result = run(repo, "--list-missing")
    assert result.returncode == 0
    assert "imagewin/ibuf8.cc all    # imagewin/Makefile.am" in result.stdout.splitlines()


# --------------------------------------------------------------------------
# Companions

SCALED = "imagewin/ibuf8_scaled.cc"
PAIR = "imagewin/ibuf8.cc  imagewin/ibuf8_scaled.cc    # the hooks in ibuf8.cc call it\n"
COMMON_WITH_SCALED = MAKEFILE_COMMON.replace("imagewin/ibuf8.o", "imagewin/ibuf8.o imagewin/ibuf8_scaled.o")


def add_scaled(repo, makefile_am_line="libimagewin_la_SOURCES += ibuf8_scaled.cc\n", common=True):
    """Creates ibuf8_scaled.cc and lists it in imagewin/Makefile.am, in the three projects (with
    an Xcode target "Exult" that builds both files) and, with common, next to each
    imagewin/ibuf8.o of Makefile.common."""
    (repo / SCALED).write_text("// new\n")
    with open(repo / "imagewin" / "Makefile.am", "a") as makefile:
        makefile.write(makefile_am_line)
    files = Repo.FILES + [SCALED]
    repo.write_projects(vc=files, filters=files, xcode=files, targets={"Exult": ["imagewin/ibuf8.cc", SCALED]})
    if common:
        (repo / "Makefile.common").write_text(COMMON_WITH_SCALED)


def companion_failures(result):
    return [line for line in result.stdout.splitlines() if "must be listed with" in line]


def missing_from(where):
    return f"{SCALED}: must be listed with imagewin/ibuf8.cc, missing from {where}"


def test_companion_is_inactive_until_the_second_file_exists(repo):
    (repo / "companions.txt").write_text(PAIR)
    result = run(repo, "--strict")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "1 companion pairs (1 inactive), 0 missing companions" in result.stdout


def test_companion_must_be_listed_wherever_the_first_file_is(repo):
    (repo / "companions.txt").write_text(PAIR)
    add_scaled(repo, common=False)
    result = run(repo)
    assert result.returncode == 1
    # A variable and a rule of Makefile.common name imagewin/ibuf8.o.
    assert companion_failures(result) == [missing_from("Makefile.common: IMAGEWIN_OBJS"), missing_from("Makefile.common: rule $(GEN)")]
    (repo / "Makefile.common").write_text(COMMON_WITH_SCALED)
    result = run(repo, "--strict")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "1 companion pairs (0 inactive), 0 missing companions" in result.stdout


def test_companion_needs_the_same_conditionals_or_fewer(repo):
    (repo / "companions.txt").write_text(PAIR)
    # Inside "if BUILD_EXULT" only, while ibuf8.cc is listed unconditionally.
    add_scaled(repo, makefile_am_line="if BUILD_EXULT\nlibimagewin_la_SOURCES += ibuf8_scaled.cc\nendif\n")
    result = run(repo)
    assert result.returncode == 1
    assert companion_failures(result) == [missing_from("imagewin/Makefile.am: libimagewin_la_SOURCES")]
    # In the else branch of the first file's conditional: still missing where the first file is.
    (repo / "imagewin" / "Makefile.am").write_text(
        "if HAVE_SDL\n" + MAKEFILE_AM + "else\nlibimagewin_la_SOURCES = ibuf8_scaled.cc\nendif\n"
    )
    assert companion_failures(run(repo)) == [missing_from("imagewin/Makefile.am: libimagewin_la_SOURCES (inside if HAVE_SDL)")]
    # Fewer conditionals than the first file: listed wherever the first file is.
    (repo / "imagewin" / "Makefile.am").write_text("if HAVE_SDL\n" + MAKEFILE_AM + "endif\nlibimagewin_la_SOURCES += ibuf8_scaled.cc\n")
    result = run(repo, "--strict")
    assert result.returncode == 0, result.stdout + result.stderr


def test_companion_in_every_project_target_and_makefile_mingw_rule(repo):
    (repo / "companions.txt").write_text(PAIR)
    add_scaled(repo)
    files = Repo.FILES + [SCALED]
    # The Xcode project references the new file, but its target does not build it.
    repo.write_projects(vc=files, filters=files, xcode=files, targets={"Exult": ["imagewin/ibuf8.cc"]})
    studio = repo / "msvcstuff" / "vs2019" / "exult_studio"
    studio.mkdir()
    (studio / "exult_studio.vcxproj").write_text(vcxproj(["../imagewin/ibuf8.cc"]))
    # One link line names the object as a prerequisite, the other only in its recipe.
    (repo / "Makefile.mingw").write_text(
        "u7shp$(EXEEXT) : tools/u7shp.o $(FILE_OBJS) imagewin/ibuf8.o\n\t$(CXX) -o $@ $+\n"
        "exult_shp$(EXEEXT): tools/exult_shp.o\n\t$(CXX) -o $@ tools/exult_shp.o imagewin/ibuf8.o -lpng\n"
    )
    result = run(repo)
    assert result.returncode == 1
    assert companion_failures(result) == [
        missing_from("Makefile.mingw: rule exult_shp$(EXEEXT)"),
        missing_from("Makefile.mingw: rule u7shp$(EXEEXT)"),
        missing_from("ios/Exult.xcodeproj/project.pbxproj: target Exult"),
        missing_from("msvcstuff/vs2019/exult_studio/exult_studio.vcxproj"),
    ]
    repo.write_projects(vc=files, filters=files, xcode=files, targets={"Exult": ["imagewin/ibuf8.cc", SCALED]})
    (studio / "exult_studio.vcxproj").write_text(vcxproj(["../imagewin/ibuf8.cc", "../imagewin/ibuf8_scaled.cc"]))
    (repo / "Makefile.mingw").write_text(
        "u7shp$(EXEEXT) : tools/u7shp.o imagewin/ibuf8.o imagewin/ibuf8_scaled.o\n\t$(CXX) -o $@ $+\n"
        "exult_shp$(EXEEXT): tools/exult_shp.o\n\t$(CXX) -o $@ tools/exult_shp.o imagewin/ibuf8.o imagewin/ibuf8_scaled.o\n"
    )
    result = run(repo, "--strict")
    assert result.returncode == 0, result.stdout + result.stderr


def test_companion_not_needed_in_the_first_file_own_dependency_rule(repo):
    (repo / "companions.txt").write_text(PAIR)
    add_scaled(repo)
    # The rule of the object itself names its source, as "shapeid.o : shapeid.cc ..." does.
    with open(repo / "Makefile.common", "a") as makefile:
        makefile.write("imagewin/ibuf8.o : imagewin/ibuf8.cc imagewin/ibuf8.h\n")
    result = run(repo, "--strict")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "1 companion pairs (0 inactive), 0 missing companions" in result.stdout


def test_companion_whose_first_file_is_listed_nowhere(repo):
    (repo / "companions.txt").write_text("imagewin/gone.cc  imagewin/gone_scaled.cc\n")
    result = run(repo)
    assert result.returncode == 0
    assert "imagewin/gone.cc is listed nowhere" in result.stderr
    assert run(repo, "--strict").returncode == 1
    (repo / "companions.txt").write_text("imagewin/ibuf8.cc\n")
    assert run(repo).returncode == 2
