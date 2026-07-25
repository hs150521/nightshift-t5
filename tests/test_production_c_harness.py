from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = (
    ROOT / "tests" / "c" / "test_link_session.c",
    ROOT / "src" / "link_session.c",
    ROOT / "src" / "request_cache.c",
    ROOT / "src" / "t5_protocol.c",
)


def _run_msvc(output: Path) -> subprocess.CompletedProcess[str] | None:
    vc_root = Path(
        r"C:\Program Files (x86)\Microsoft Visual Studio"
        r"\2019\BuildTools\VC\Tools\MSVC"
    )
    windows_root = Path(r"C:\Program Files (x86)\Windows Kits\10")
    vc_versions = sorted(
        (path for path in vc_root.glob("*") if path.is_dir()),
        reverse=True,
    )
    sdk_versions = sorted(
        (path for path in (windows_root / "Include").glob("*")
         if path.is_dir()),
        reverse=True,
    )
    if not vc_versions or not sdk_versions:
        return None
    vc = vc_versions[0]
    sdk_version = sdk_versions[0].name
    compiler = vc / "bin" / "Hostx64" / "x64" / "cl.exe"
    if not compiler.exists():
        return None

    env = os.environ.copy()
    env["PATH"] = os.pathsep.join(
        (
            str(compiler.parent),
            str(windows_root / "bin" / sdk_version / "x64"),
            env.get("PATH", ""),
        )
    )
    env["INCLUDE"] = os.pathsep.join(
        (
            str(vc / "include"),
            str(windows_root / "Include" / sdk_version / "ucrt"),
            str(windows_root / "Include" / sdk_version / "shared"),
            str(windows_root / "Include" / sdk_version / "um"),
        )
    )
    env["LIB"] = os.pathsep.join(
        (
            str(vc / "lib" / "x64"),
            str(windows_root / "Lib" / sdk_version / "ucrt" / "x64"),
            str(windows_root / "Lib" / sdk_version / "um" / "x64"),
        )
    )
    return subprocess.run(
        [
            str(compiler),
            "/nologo",
            "/std:c11",
            "/W4",
            f"/I{ROOT / 'src'}",
            f"/I{ROOT / 'include'}",
            *(str(source) for source in SOURCES),
            f"/Fe:{output}",
        ],
        cwd=output.parent,
        env=env,
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=True,
        check=False,
    )


def _run_posix_compiler(output: Path) -> subprocess.CompletedProcess[str] | None:
    compiler = next(
        (
            shutil.which(name)
            for name in ("cc", "gcc", "clang")
            if shutil.which(name)
        ),
        None,
    )
    if compiler is None:
        return None
    return subprocess.run(
        [
            compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{ROOT / 'src'}",
            f"-I{ROOT / 'include'}",
            *(str(source) for source in SOURCES),
            "-o",
            str(output),
        ],
        cwd=output.parent,
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=True,
        check=False,
    )


class ProductionCHarnessTests(unittest.TestCase):
    def test_production_session_cache_and_codec_modules(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nightshift-t5-c-") as temp:
            output = Path(temp) / (
                "link_session_tests.exe" if os.name == "nt"
                else "link_session_tests"
            )
            compile_result = (
                _run_msvc(output)
                if os.name == "nt"
                else _run_posix_compiler(output)
            )
            if compile_result is None:
                self.skipTest("no supported host C compiler found")
            self.assertEqual(
                compile_result.returncode,
                0,
                compile_result.stdout + compile_result.stderr,
            )
            run_result = subprocess.run(
                [str(output)],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(
                run_result.returncode,
                0,
                run_result.stdout + run_result.stderr,
            )
            self.assertIn(
                "production link_session C tests: PASS",
                run_result.stdout,
            )


if __name__ == "__main__":
    unittest.main()
