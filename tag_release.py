import subprocess
import sys
import re


def run(cmd):
    result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    return result.stdout.strip(), result.returncode


def abort(msg):
    print(f"Error: {msg}")
    sys.exit(1)


def main():
    branch, _ = run("git branch --show-current")
    if branch != "master":
        abort(f"Must be on 'master' branch (currently on '{branch}')")

    # Get the latest version tag
    tags, _ = run("git tag -l 'v[0-9]*.[0-9]*.[0-9]*' --sort=-v:refname")

    if not tags:
        new_tag = "v1.0.0"
    else:
        latest = tags.splitlines()[0]
        m = re.match(r"v(\d+)\.(\d+)\.(\d+)", latest)
        if not m:
            abort(f"Cannot parse tag '{latest}'")
        major, minor, patch = int(m.group(1)), int(m.group(2)), int(m.group(3))
        new_tag = f"v{major}.{minor + 1}.{patch}"

    print(f"Current latest tag: {tags.splitlines()[0] if tags else '(none)'}")
    print(f"Creating tag: {new_tag}")

    _, rc = run(f"git tag {new_tag}")
    if rc != 0:
        abort(f"Failed to create tag {new_tag}")

    _, rc = run(f"git push origin {new_tag}")
    if rc != 0:
        abort(f"Failed to push tag {new_tag}")

    print(f"Tag {new_tag} pushed to origin/master")


if __name__ == "__main__":
    main()
