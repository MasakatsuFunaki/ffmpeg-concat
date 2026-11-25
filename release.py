#!/usr/bin/env python3
"""Release automation for ffmpeg-tools (single-branch 'main' + GitHub Releases)."""

import argparse
import logging
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile

logging.basicConfig(level=logging.INFO, format="%(message)s")

PROJECT_ROOT = os.path.dirname(os.path.realpath(__file__))
IS_WINDOWS = platform.system() == "Windows"
INSTALL_DIR = os.path.join(PROJECT_ROOT, "install", "win" if IS_WINDOWS else "linux")
SEMVER_TAG_RE = re.compile(r"^v(\d+)\.(\d+)\.(\d+)$")


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def abort(msg):
    logging.error(f"Error: {msg}")
    sys.exit(1)


def run(cmd, check=True, capture=True):
    """Execute a command safely as an argument list (never via a shell)."""
    logging.info(f"> {' '.join(cmd)}")
    if capture:
        result = subprocess.run(
            cmd,
            cwd=PROJECT_ROOT,
            capture_output=True,
            text=True,
        )
        if result.stdout.strip():
            logging.info(result.stdout.strip())
        if result.stderr.strip():
            logging.info(result.stderr.strip())
    else:
        result = subprocess.run(cmd, cwd=PROJECT_ROOT)

    if check and result.returncode != 0:
        abort(f"Command failed with exit code {result.returncode}: {' '.join(cmd)}")
    return result


def run_mutating(cmd, dry_run, check=True, capture=True):
    """Run a mutating command, or log the planned command in --dry-run mode."""
    if dry_run:
        logging.info(f"[dry-run] Would run: {' '.join(cmd)}")
        return None
    return run(cmd, check=check, capture=capture)


def host_archive_suffix():
    """Return (<os_arch_label>, <shutil.make_archive format>, <file extension>)."""
    if IS_WINDOWS:
        return "windows-x64", "zip", ".zip"
    machine = platform.machine().lower()
    if machine in ("x86_64", "amd64"):
        arch = "x64"
    elif machine in ("aarch64", "arm64"):
        arch = "arm64"
    else:
        arch = machine or "x64"
    return f"linux-{arch}", "gztar", ".tar.gz"


def compute_next_tag(bump):
    """Compute next semver tag from existing vX.Y.Z tags (starting at v1.0.0)."""
    result = run(["git", "tag", "--list", "v*.*.*"], check=False)
    versions = []
    for line in result.stdout.splitlines():
        match = SEMVER_TAG_RE.match(line.strip())
        if match:
            versions.append(tuple(int(part) for part in match.groups()))

    if not versions:
        return "v1.0.0"

    major, minor, patch = max(versions)
    if bump == "major":
        return f"v{major + 1}.0.0"
    if bump == "minor":
        return f"v{major}.{minor + 1}.0"
    return f"v{major}.{minor}.{patch + 1}"


def tag_exists_locally(tag):
    result = run(["git", "rev-parse", "-q", "--verify", f"refs/tags/{tag}"], check=False)
    return result.returncode == 0


# ---------------------------------------------------------------------------
# Release workflow steps
# ---------------------------------------------------------------------------

def preflight_checks(require_gh, dry_run):
    branch = run(["git", "branch", "--show-current"]).stdout.strip()
    if branch != "main":
        abort(f"Must be on 'main' branch to release (currently on '{branch}').")

    status = run(["git", "status", "--porcelain"]).stdout.strip()
    if status:
        abort("Working tree has uncommitted changes. Commit or stash them before releasing.")

    if require_gh and not dry_run:
        if shutil.which("gh") is None:
            abort("GitHub CLI 'gh' not found. Install https://cli.github.com and run 'gh auth login'.")
        if run(["gh", "auth", "status"], check=False).returncode != 0:
            abort("GitHub CLI is not authenticated. Run 'gh auth login' first.")


def package_local_archive(tag, dry_run):
    """Build + install locally and package install/<os>/ into the release archive."""
    build_cmd = [sys.executable, os.path.join(PROJECT_ROOT, "build.py"), "--install"]
    logging.info("\n=== Building & installing local deliverables ===")
    run_mutating(build_cmd, dry_run=dry_run, capture=False)

    os_arch, fmt, ext = host_archive_suffix()
    base_name = f"ffmpeg-tools-{tag}-{os_arch}"
    base_path = os.path.join(tempfile.gettempdir(), base_name)
    expected_archive = base_path + ext

    if dry_run:
        logging.info(f"[dry-run] Would package {INSTALL_DIR} -> {expected_archive}")
        return expected_archive

    if not (os.path.isdir(INSTALL_DIR) and os.listdir(INSTALL_DIR)):
        abort(f"Install directory '{INSTALL_DIR}' is empty. Did 'build.py --install' succeed?")

    archive = shutil.make_archive(base_path, fmt, root_dir=INSTALL_DIR)
    size_mib = os.path.getsize(archive) / (1024 * 1024)
    logging.info(f"Packaged {archive} ({size_mib:.1f} MiB)")
    return archive


def push_main_and_tag(tag, notes, allow_existing_tag, dry_run):
    """Push 'main' and annotated '<tag>' to 'origin'."""
    logging.info("\n=== Pushing 'main' to origin ===")
    run_mutating(["git", "push", "origin", "main"], dry_run=dry_run)

    if tag_exists_locally(tag):
        if not allow_existing_tag:
            abort(f"Tag '{tag}' already exists.")
        logging.info(f"Tag '{tag}' already exists locally; skipping tag creation.")
        return

    logging.info(f"\n=== Creating and pushing annotated tag {tag} ===")
    run_mutating(["git", "tag", "-a", tag, "-m", f"release: {notes}"], dry_run=dry_run)
    run_mutating(["git", "push", "origin", tag], dry_run=dry_run)


def upload_local_release(archive, tag, notes, dry_run):
    """Create or update the GitHub Release with the locally built archive."""
    logging.info(f"\n=== Uploading local asset to GitHub Release {tag} ===")
    if dry_run:
        logging.info(
            f"[dry-run] Would upload '{os.path.basename(archive)}' to GitHub Release {tag} "
            "via 'gh release upload --clobber' or 'gh release create'"
        )
        return

    if run(["gh", "release", "view", tag], check=False).returncode == 0:
        run(["gh", "release", "upload", tag, archive, "--clobber"])
    else:
        run([
            "gh", "release", "create", tag, archive,
            "--verify-tag",
            "--title", tag,
            "--notes", f"release: {notes}",
        ])
    logging.info(f"\nRelease {tag} updated with {os.path.basename(archive)}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Cut a semantic-version release from 'main'. By default, pushes 'main' and an "
            "annotated tag to 'origin' so GitHub Actions builds and publishes both Windows "
            "and Linux release archives + SHA256SUMS.txt."
        )
    )
    parser.add_argument(
        "--tag",
        default=None,
        help="Explicit release tag matching vX.Y.Z (e.g., --tag v2.0.0)",
    )
    parser.add_argument(
        "--bump",
        choices=["patch", "minor", "major"],
        default="patch",
        help="Version component to bump when --tag is omitted (default: patch; starts at v1.0.0)",
    )
    parser.add_argument(
        "--notes",
        default=None,
        help="Release notes text (default: latest git commit subject)",
    )
    parser.add_argument(
        "--local",
        action="store_true",
        help=(
            "Also build locally ('python build.py --install'), package install/<os>/, "
            "and upload the archive directly via GitHub CLI ('gh')"
        ),
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print every planned step without mutating git, build outputs, or GitHub",
    )
    return parser.parse_args()


def main():
    args = parse_args()

    if args.tag is not None and not SEMVER_TAG_RE.match(args.tag):
        abort(f"Invalid --tag '{args.tag}': expected semantic version format 'vX.Y.Z' (e.g., v2.0.0).")

    preflight_checks(require_gh=args.local, dry_run=args.dry_run)

    logging.info("\n=== Syncing tags from origin ===")
    run_mutating(["git", "fetch", "--tags", "origin"], dry_run=args.dry_run)

    tag = args.tag if args.tag is not None else compute_next_tag(args.bump)
    if not args.local and tag_exists_locally(tag):
        abort(f"Tag '{tag}' already exists. Choose a new --tag or --bump.")

    notes = args.notes if args.notes is not None else run(
        ["git", "log", "-1", "--format=%s"]
    ).stdout.strip()

    logging.info(f"\nRelease plan: tag={tag}, mode={'local' if args.local else 'ci'}, notes={notes!r}")

    if args.local:
        archive = package_local_archive(tag, dry_run=args.dry_run)
        push_main_and_tag(tag, notes, allow_existing_tag=True, dry_run=args.dry_run)
        upload_local_release(archive, tag, notes, dry_run=args.dry_run)
    else:
        push_main_and_tag(tag, notes, allow_existing_tag=False, dry_run=args.dry_run)
        if args.dry_run:
            logging.info(
                f"[dry-run] Pushing {tag} to origin would trigger GitHub Actions ('CI & Release') "
                "to build and publish Windows & Linux archives + SHA256SUMS.txt."
            )
        else:
            logging.info(
                f"\nTag {tag} pushed to origin. GitHub Actions ('CI & Release') will now build "
                "and publish the Windows & Linux release archives."
            )


if __name__ == "__main__":
    main()
