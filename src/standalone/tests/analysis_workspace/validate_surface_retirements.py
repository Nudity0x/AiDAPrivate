import argparse
import json
import re
import sys
from pathlib import Path

ALLOWED_KINDS = [
    "mcp_registration",
    "mcp_resource",
    "center_view",
    "ui_action",
    "ui_shortcut_key",
    "source_contract",
    "test_lab_feature",
]

LEGACY_TOKENS = [
    "ImGui::",
    "imgui.h",
    "imgui_internal.h",
    "core/ui/design_system",
    "core/ui/theme.hpp",
    "core/ui/components.hpp",
    "core/ui/empty_state",
    "core/ui/ui_anim",
    "core/ui/quick_open",
    "core/ui/hub_strip",
    "core/ui/skeleton",
    "core/ui/responsive",
    "core/ui/brand",
    "core/ui/avatar",
    "core/ui/application_view_registry",
    "core/ui/view_registry",
    "core/ui/workspace_layout",
    "preview/",
]

REQUIRED_ROW_FIELDS = ["kind", "id", "reason", "plan", "replacement"]
SCAN_SUFFIXES = {".cpp", ".h", ".hpp"}
QT_ROOT_REL = "src/standalone/src/qt"
TESTLAB_QT_ROOT_REL = "src/standalone/src/core/testlab/qt"
REPLACEMENT_PREFIXES = ("src/standalone/src/qt/", "src/standalone/src/core/testlab/qt/")
LEDGER_REL = "src/standalone/tests/analysis_workspace/surface_retirements_qt.json"
GENERATOR_REL = "src/standalone/tests/analysis_workspace/generate_surface_manifest.ps1"


def discover_repo_root(start):
    for candidate in (start, *start.parents):
        if (candidate / "src" / "standalone").is_dir():
            return candidate
    raise SystemExit(f"FAIL repo root not found walking up from {start}")


def read_source_text(path):
    return path.read_text(encoding="utf-8-sig", errors="replace")


def check_ledger_shape(ledger_path):
    try:
        document = json.loads(ledger_path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as error:
        return None, [f"ledger unreadable or invalid JSON: {ledger_path}: {error}"]
    if not isinstance(document, dict):
        return None, ["ledger root must be an object"]
    rows = document.get("retirements")
    if not isinstance(rows, list):
        return None, ['ledger root lacks a "retirements" array']
    failures = []
    seen = set()
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            failures.append(f"retirements[{index}] is not an object")
            continue
        for field in REQUIRED_ROW_FIELDS:
            value = row.get(field)
            if not isinstance(value, str) or not value.strip():
                failures.append(f"retirements[{index}] lacks non-empty '{field}'")
        kind = row.get("kind")
        row_id = row.get("id")
        if isinstance(kind, str) and kind.strip() and kind not in ALLOWED_KINDS:
            failures.append(f"retirements[{index}] has unknown kind '{kind}'")
        if (
            isinstance(kind, str)
            and isinstance(row_id, str)
            and kind.strip()
            and row_id.strip()
        ):
            key = (kind, row_id)
            if key in seen:
                failures.append(f"duplicate retirement row for '{kind}/{row_id}'")
            seen.add(key)
    return rows, failures


def check_replacements(repo_root, rows):
    failures = []
    for row in rows:
        if not isinstance(row, dict):
            continue
        replacement = row.get("replacement")
        if not isinstance(replacement, str) or not replacement.strip():
            continue
        if replacement == "none":
            continue
        label = f"{row.get('kind')}/{row.get('id')}"
        if not replacement.startswith(REPLACEMENT_PREFIXES):
            failures.append(
                f"replacement for '{label}' is not 'none' or a path under "
                f"src/standalone/src/qt/ or src/standalone/src/core/testlab/qt/: '{replacement}'"
            )
            continue
        if not (repo_root / replacement).is_file():
            failures.append(
                f"replacement for '{label}' does not exist on disk: '{replacement}'"
            )
    return failures


def enumerate_scan_files(repo_root):
    found_dirs = []
    files = []
    for rel in (QT_ROOT_REL, TESTLAB_QT_ROOT_REL):
        root = repo_root / rel
        if not root.is_dir():
            continue
        found_dirs.append(rel)
        for path in sorted(root.rglob("*")):
            if path.is_file() and path.suffix.lower() in SCAN_SUFFIXES:
                files.append(path)
    return found_dirs, files


def zero_reference_sweep(repo_root, files, needles):
    failures = []
    for path in files:
        rel = path.relative_to(repo_root).as_posix()
        for line_number, line in enumerate(read_source_text(path).splitlines(), 1):
            for needle in needles:
                if needle in line:
                    failures.append(f"zero-reference hit {rel}:{line_number}: '{needle}'")
    return failures


def cross_check_generator(ps1_path):
    try:
        text = ps1_path.read_text(encoding="utf-8-sig", errors="replace")
    except OSError as error:
        return [f"generator unreadable: {ps1_path}: {error}"]
    failures = []
    if "$qtSourceRoot = Join-Path $RepositoryRoot 'src\\standalone\\src\\qt'" not in text:
        failures.append("generator no longer anchors $qtSourceRoot at src\\standalone\\src\\qt")
    if "Get-ChildItem -LiteralPath $qtSourceRoot -Recurse -File" not in text:
        failures.append("generator no longer recurses $qtSourceRoot for source files")
    if "$_.Extension -in @('.cpp', '.h', '.hpp')" not in text:
        failures.append("generator qt enumeration no longer filters .cpp/.h/.hpp")
    match = re.search(r"\$allowedRetirementKinds\s*=\s*@\((.*?)\)", text, re.S)
    if not match:
        failures.append("generator lacks an $allowedRetirementKinds list")
    else:
        kinds = re.findall(r"'([^']*)'", match.group(1))
        if kinds != ALLOWED_KINDS:
            failures.append(
                f"generator kind set {kinds} does not equal validator kind set {ALLOWED_KINDS}"
            )
    return failures


def main():
    parser = argparse.ArgumentParser(
        description="Validate the Qt surface retirement ledger and qt source tree."
    )
    parser.add_argument(
        "--repo-root",
        default=None,
        help="repository root (default: discovered by walking up from the script)",
    )
    parser.add_argument(
        "--ledger",
        default=None,
        help=f"ledger path, absolute or repo-relative (default: {LEDGER_REL})",
    )
    args = parser.parse_args()
    script_dir = Path(__file__).resolve().parent
    repo_root = (
        Path(args.repo_root).resolve()
        if args.repo_root
        else discover_repo_root(script_dir)
    )
    ledger_path = Path(args.ledger) if args.ledger else repo_root / LEDGER_REL
    if not ledger_path.is_absolute():
        ledger_path = repo_root / ledger_path
    failures = []
    rows, shape_failures = check_ledger_shape(ledger_path)
    failures.extend(shape_failures)
    ids = []
    scan_dirs = []
    scan_files = []
    if rows is not None:
        failures.extend(check_replacements(repo_root, rows))
        ids = [
            row["id"]
            for row in rows
            if isinstance(row, dict)
            and isinstance(row.get("id"), str)
            and row["id"].strip()
        ]
        if not (repo_root / QT_ROOT_REL).is_dir():
            failures.append(f"qt shell tree missing: {QT_ROOT_REL}")
        else:
            scan_dirs, scan_files = enumerate_scan_files(repo_root)
            failures.extend(
                zero_reference_sweep(repo_root, scan_files, ids + LEGACY_TOKENS)
            )
    failures.extend(cross_check_generator(repo_root / GENERATOR_REL))
    if failures:
        for failure in failures:
            print(f"FAIL {failure}")
        return 1
    needle_count = len(ids) + len(LEGACY_TOKENS)
    print(f"rows checked: {len(rows)}")
    print(f"files scanned: {len(scan_files)}")
    print(f"qt dirs found: {', '.join(scan_dirs)}")
    print(
        f"PASS surface retirements ledger valid; {len(rows)} rows; "
        f"{len(scan_files)} qt source files clean of {needle_count} needles"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
