#!/usr/bin/env python3
"""West launcher with git revision parsing compatibility fallbacks.

Some Git/West combinations fail to resolve expressions like "rev^{commit}".
This wrapper patches west.manifest.Project.sha at runtime to try safe fallbacks.
"""

from __future__ import annotations

import subprocess
import sys


def _patch_west_sha() -> None:
    import west.manifest as manifest
    import west.app.project as project_mod

    original_git = manifest.Project.git
    qual_manifest_rev = project_mod.QUAL_MANIFEST_REV

    def _resolve_with_fallbacks(project, rev: str, cwd=None) -> str:
        candidates = []
        if rev.endswith("^{commit}"):
            base = rev[: -len("^{commit}")]
            candidates.extend([f"{base}^{{}}", base])
        else:
            candidates.extend([f"{rev}^{{commit}}", f"{rev}^{{}}", rev])

        last_error = None
        for candidate in candidates:
            try:
                cp = original_git(
                    project,
                    ["rev-parse", candidate],
                    capture_stdout=True,
                    cwd=cwd,
                    capture_stderr=True,
                )
                return cp.stdout.decode("ascii").strip()
            except subprocess.CalledProcessError as exc:
                last_error = exc

        if last_error is not None:
            raise last_error
        raise RuntimeError("Failed to resolve git revision")

    def compat_git(
        self,
        cmd,
        extra_args=(),
        capture_stdout=False,
        capture_stderr=False,
        check=True,
        cwd=None,
    ):
        # Git for Windows can reject update-ref values like rev^{commit};
        # resolve to a concrete SHA before passing to update-ref.
        if isinstance(cmd, list) and cmd and cmd[0] == "update-ref":
            if len(cmd) >= 2 and cmd[1] == "-d":
                pass
            elif cmd and isinstance(cmd[-1], str) and "^{commit}" in cmd[-1]:
                rewritten = list(cmd)
                rewritten[-1] = _resolve_with_fallbacks(self, rewritten[-1], cwd=cwd)
                cmd = rewritten

        return original_git(
            self,
            cmd,
            extra_args=extra_args,
            capture_stdout=capture_stdout,
            capture_stderr=capture_stderr,
            check=check,
            cwd=cwd,
        )

    manifest.Project.git = compat_git  # type: ignore[assignment]

    def compat_sha(self, rev: str, cwd=None) -> str:  # type: ignore[override]
        return _resolve_with_fallbacks(self, rev, cwd=cwd)

    manifest.Project.sha = compat_sha  # type: ignore[assignment]

    def compat_update_manifest_rev(project, new_manifest_rev):
        resolved = project.sha(new_manifest_rev)
        project.git(
            [
                "update-ref",
                "-m",
                f"west update: moving to {new_manifest_rev}",
                qual_manifest_rev,
                resolved,
            ]
        )

    project_mod._update_manifest_rev = compat_update_manifest_rev


def main(argv: list[str]) -> int:
    _patch_west_sha()
    from west.app.main import main as west_main

    try:
        west_main(argv)
        return 0
    except SystemExit as exc:
        code = exc.code
        if isinstance(code, int):
            return code
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
