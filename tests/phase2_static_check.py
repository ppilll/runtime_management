"""Source-only checks; uses Python's standard library, no CMake/compiler.

This checks structure and test wiring, not C++ types, linking or behavior.
Run from any directory: python tests/phase2_static_check.py
"""
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parent.parent


def require(condition, message):
    if not condition:
        raise ValueError(message)


def masked_source(source):
    """Blank comments/literals before matching delimiters and test functions."""
    output = list(source)
    index = 0
    while index < len(source):
        start = index
        if source.startswith("//", index):
            end = source.find("\n", index)
            index = len(source) if end < 0 else end
        elif source.startswith("/*", index):
            end = source.find("*/", index + 2)
            require(end >= 0, "unterminated block comment")
            index = end + 2
        else:
            raw = re.match(r'R"([^\s()\\]{0,16})\(', source[index:])
            if raw:
                closing = ")" + raw.group(1) + '"'
                end = source.find(closing, index + raw.end())
                require(end >= 0, "unterminated raw string")
                index = end + len(closing)
            elif source[index] in "\"'":
                quote = source[index]
                index += 1
                while index < len(source) and source[index] != quote:
                    require(source[index] != "\n", "newline in quoted literal")
                    index += 2 if source[index] == "\\" else 1
                require(index < len(source), "unterminated quoted literal")
                index += 1
            else:
                index += 1
                continue
        for position in range(start, index):
            if source[position] != "\n":
                output[position] = " "
    return "".join(output)


def check_source(path):
    source = path.read_text(encoding="utf-8-sig")
    require(not re.search(r"^(?:<{7}|={7}|>{7})(?:\s|$)", source, re.M),
            "merge conflict marker")
    for include in re.findall(r'^\s*#\s*include\s+"([^"]+)"', source, re.M):
        candidates = [path.parent / include, ROOT / "include" / include]
        require(any(candidate.is_file() for candidate in candidates),
                "unresolved local include: " + include)
    masked = masked_source(source)
    stack = []
    closing = {"}": "{", ")": "(", "]": "["}
    for offset, char in enumerate(masked):
        if char in "{([":
            stack.append(char)
        elif char in closing:
            line = masked.count("\n", 0, offset) + 1
            require(stack and stack.pop() == closing[char], f"unmatched {char} at line {line}")
    require(not stack, "unclosed delimiter")
    if path.parent == ROOT / "tests" and path.suffix == ".cpp":
        main = re.search(r"\bint\s+main\s*\(", masked)
        require(main is not None, "test executable has no main")
        tests = re.findall(r"\bvoid\s+(test_\w+)\s*\(", masked)
        for name in tests:
            require(re.search(r"\b" + name + r"\s*\(", masked[main.end():]),
                    "test is not invoked by main: " + name)
        return len(tests)
    return 0


def check_registration():
    cmake = (ROOT / "tests" / "CMakeLists.txt").read_text(encoding="utf-8-sig")
    cmake = re.sub(r"#[^\n]*", "", cmake)
    targets = {}
    for target, source in re.findall(r"add_executable\(\s*(\w+)\s+([^\s)]+)\s*\)", cmake):
        require(target not in targets, "duplicate test executable: " + target)
        require((ROOT / "tests" / source).is_file(), "missing test source: " + source)
        targets[target] = source
    registrations = re.findall(r"add_test\(\s*NAME\s+(\w+)\s+COMMAND\s+(\w+)", cmake)
    require(len({name for name, _ in registrations}) == len(registrations), "duplicate CTest name")
    for name, target in registrations:
        require(target in targets, "unresolved test target: " + name)
    for target in targets:
        require(any(registered == target for _, registered in registrations),
                "executable is not registered: " + target)
        require(re.search(r"target_link_libraries\(\s*" + target + r"\s+PRIVATE\s+", cmake),
                "test target has no library linkage: " + target)
    for name in ("phase2_lifecycle_integration", "phase2_runtime_shutdown_regression"):
        require((name, "phase2_integration_tests") in registrations, "missing Phase 2 registration: " + name)
    require(re.search(r"COMMAND\s+phase2_integration_tests\s+--shutdown-regression\s*\)", cmake),
            "shutdown regression option is not wired")
    require(re.search(r"set_tests_properties\(\s*phase2_lifecycle_integration\s+"
                      r"phase2_runtime_shutdown_regression\s+PROPERTIES\s+TIMEOUT\s+60\s*\)", cmake),
            "Phase 2 integration timeout is not wired")
    require(not re.search(r"\b(?:WILL_FAIL|DISABLED)\s+(?:TRUE|ON|1)\b", cmake, re.I),
            "test failure is masked by CTest properties")
    return len(targets), len(registrations)


def main():
    files = sorted(path for directory in ("include", "src", "tools", "tests")
                   for path in (ROOT / directory).rglob("*")
                   if path.suffix in (".hpp", ".cpp"))
    failures = []
    scenarios = 0
    for path in files:
        try:
            scenarios += check_source(path)
        except ValueError as error:
            failures.append(f"{path.relative_to(ROOT)}: {error}")
    # git diff --check omits untracked files; cover this task's added artifacts.
    for name in ("phase2_integration_tests.cpp", "phase2_static_check.py", "phase2_review.md"):
        path = ROOT / "tests" / name
        source = path.read_text(encoding="utf-8-sig")
        for line, content in enumerate(source.splitlines(), 1):
            if content != content.rstrip():
                failures.append(f"tests/{name}:{line}: trailing whitespace")
        if not source.endswith("\n"):
            failures.append(f"tests/{name}: missing final newline")
    try:
        targets, registrations = check_registration()
    except ValueError as error:
        failures.append(f"tests/CMakeLists.txt: {error}")
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"PASS: {len(files)} C++ files; local includes, delimiters, comments/literals, conflict markers")
    print(f"PASS: {scenarios} test functions invoked by main; {targets} targets, {registrations} CTest entries")
    print("PASS: added test/checker/report whitespace and final newlines")
    print("LIMIT: source structure only; C++ type/link correctness and runtime behavior are unverified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
