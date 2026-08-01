"""Privacy contract for the live GraphQL repository example."""

from __future__ import annotations

import ast
import hashlib
from collections import Counter


EXPECTED_OUTPUT_KEYS = [
    "artifact",
    "completed",
    "extension",
    "relation",
    "required_values_present",
    "schema",
]
EXPECTED_PUBLIC_SCHEMA = [
    ["id", "VARCHAR"],
    ["full_name", "VARCHAR"],
    ["owner_login", "VARCHAR"],
    ["stars", "BIGINT"],
    ["primary_language", "VARCHAR"],
    ["private", "BOOLEAN"],
    ["archived", "BOOLEAN"],
    ["updated_at", "VARCHAR"],
]
EXPECTED_OUTPUT_EXPRESSIONS = {
    "artifact": "artifact.name",
    "completed": "True",
    "extension": (
        "{'install_mode': 'NOT_INSTALLED', 'installed': False, "
        "'loaded': True, 'name': 'cuac', 'version': '0.1.0'}"
    ),
    "relation": "'github.viewer_repository_metrics'",
    "required_values_present": "True",
    "schema": repr(EXPECTED_PUBLIC_SCHEMA),
}
EXPECTED_ASSIGNMENT_EXPRESSIONS = {
    "artifact": "pathlib.Path(args.artifact).resolve(strict=True)",
    "package_root": (
        "(pathlib.Path(__file__).resolve().parents[1] / "
        "'connectors/github').resolve(strict=True)"
    ),
    "sql_path": "pathlib.Path(__file__).with_name('viewer-repository-metrics.sql')",
    "connection": "duckdb.connect(config={'allow_unsigned_extensions': 'true'})",
    "statements": (
        "connection.extract_statements(example_sql(sql_path, package_root))"
    ),
    "described": "connection.execute(statements[1]).fetchall()",
    "schema": "[(row[0], row[1]) for row in described]",
    "explanation": (
        "'\\n'.join(str(value) for row in "
        "connection.execute(statements[2]).fetchall() for value in row)"
    ),
    "result": "connection.execute(statements[3])",
    "result_schema": (
        "[(column[0], str(column[1])) for column in result.description]"
    ),
    "completion": "result.fetchone()",
}
EXPECTED_ANNOTATED_ASSIGNMENTS = {
    "extension": ("tuple[object, ...] | None", "None"),
    "schema": ("list[tuple[str, str]]", "[]"),
    "completion": ("tuple[bool, bool, bool] | None", "None"),
}
EXPECTED_NAME_STORE_COUNTS = {
    "EXPECTED_DUCKDB": 1,
    "EXPECTED_EXTENSION": 1,
    "EXPECTED_RESULT_SCHEMA": 1,
    "EXPECTED_SCHEMA": 1,
    "EXPLAIN_MARKERS": 1,
    "PACKAGE_ROOT_LITERAL": 1,
    "source": 1,
    "args": 1,
    "artifact": 1,
    "package_root": 1,
    "column": 1,
    "completion": 2,
    "connection": 1,
    "described": 1,
    "explanation": 1,
    "extension": 2,
    "result": 1,
    "result_schema": 1,
    "row": 2,
    "schema": 2,
    "sql_path": 1,
    "statements": 1,
    "token": 5,
    "value": 1,
    "marker": 1,
    "parser": 1,
}
EXPECTED_FUNCTION_ARGUMENTS = {
    "sql_literal": ("value",),
    "example_sql": ("path", "package_root"),
    "read_token": (),
    "main": (),
}
EXPECTED_RUNNER_SOURCE_DIGEST = (
    "a1c8902788afcefde8f720d5222abf39194242378126f060dc40d1ecea9af8a9"
)
EXPECTED_EXTENSION_QUERY = (
    "SELECT extension_name, extension_version, loaded, installed, install_mode "
    "FROM duckdb_extensions() WHERE extension_name = 'cuac'"
)
EXPECTED_CONSTANTS = {
    "EXPECTED_EXTENSION": ("cuac", "0.1.0", True, False, "NOT_INSTALLED"),
    "EXPECTED_SCHEMA": [
        ("id", "VARCHAR"),
        ("full_name", "VARCHAR"),
        ("owner_login", "VARCHAR"),
        ("stars", "BIGINT"),
        ("primary_language", "VARCHAR"),
        ("private", "BOOLEAN"),
        ("archived", "BOOLEAN"),
        ("updated_at", "VARCHAR"),
    ],
    "EXPECTED_RESULT_SCHEMA": [
        ("required_values_present", "BOOLEAN"),
        ("local_limit_respected", "BOOLEAN"),
        ("local_filter_respected", "BOOLEAN"),
    ],
    "PACKAGE_ROOT_LITERAL": "'/absolute/path/to/cuac/connectors/github'",
}
EXPECTED_CALL_NAMES = {
    "ArgumentParser": 1,
    "Path": 3,
    "SystemExit": 12,
    "add_argument": 1,
    "any": 1,
    "as_posix": 2,
    "close": 1,
    "connect": 1,
    "dumps": 1,
    "execute": 8,
    "extract_statements": 1,
    "fetchall": 2,
    "fetchone": 3,
    "getpass": 1,
    "join": 1,
    "len": 1,
    "main": 1,
    "parse_args": 1,
    "print": 1,
    "read_text": 1,
    "read_token": 1,
    "replace": 2,
    "resolve": 3,
    "sql_literal": 3,
    "str": 2,
    "with_name": 1,
    "count": 1,
    "example_sql": 1,
}
EXPECTED_EXECUTE_EXPRESSIONS = (
    "connection.execute(f'LOAD {sql_literal(artifact.as_posix())}')",
    "connection.execute('PRAGMA version')",
    (
        "connection.execute('''\n"
        "            SELECT extension_name, extension_version, loaded, installed, install_mode\n"
        "            FROM duckdb_extensions()\n"
        "            WHERE extension_name = 'cuac'\n"
        "            ''')"
    ),
    "connection.execute(statements[0])",
    "connection.execute(statements[1])",
    "connection.execute(statements[2])",
    (
        "connection.execute(f'CREATE TEMPORARY SECRET github_default "
        "(TYPE cuac, PROVIDER config, TOKEN {sql_literal(token)})')"
    ),
    "connection.execute(statements[3])",
)
def _call_name(node: ast.Call) -> str:
    if isinstance(node.func, ast.Name):
        return node.func.id
    if isinstance(node.func, ast.Attribute):
        return node.func.attr
    return ""


def _same_expression(node: ast.expr, expected: str) -> bool:
    expected_node = ast.parse(expected, mode="eval").body
    return ast.dump(node, include_attributes=False) == ast.dump(
        expected_node, include_attributes=False
    )


def _assignments(tree: ast.AST) -> dict[str, list[ast.expr]]:
    values: dict[str, list[ast.expr]] = {}
    for node in ast.walk(tree):
        if not isinstance(node, ast.Assign):
            continue
        for target in node.targets:
            if isinstance(target, ast.Name):
                values.setdefault(target.id, []).append(node.value)
    return values


def _require_exact_assignment(
    assignments: dict[str, list[ast.expr]], name: str, expected: str
) -> None:
    values = assignments.get(name, [])
    if len(values) != 1 or not _same_expression(values[0], expected):
        raise AssertionError(f"GraphQL example {name} provenance drifted")


def validate_runner_source(source: str) -> None:
    """Prove that live execution rows and provider text cannot reach output."""

    tree = ast.parse(source)
    runner_digest = hashlib.sha256(source.encode("utf-8")).hexdigest()
    if runner_digest != EXPECTED_RUNNER_SOURCE_DIGEST:
        raise AssertionError("GraphQL example canonical source drifted")
    imports = [
        (alias.name, alias.asname)
        for node in tree.body
        if isinstance(node, ast.Import)
        for alias in node.names
    ]
    if imports != [
        ("argparse", None),
        ("getpass", None),
        ("json", None),
        ("pathlib", None),
        ("duckdb", None),
    ]:
        raise AssertionError("GraphQL example import authority drifted")
    import_from = [node for node in tree.body if isinstance(node, ast.ImportFrom)]
    if (
        len(import_from) != 1
        or import_from[0].module != "__future__"
        or [(alias.name, alias.asname) for alias in import_from[0].names]
        != [("annotations", None)]
    ):
        raise AssertionError("GraphQL example from-import authority drifted")

    calls = [node for node in ast.walk(tree) if isinstance(node, ast.Call)]
    call_names = Counter(_call_name(node) for node in calls)
    if dict(call_names) != EXPECTED_CALL_NAMES:
        raise AssertionError("GraphQL example closed call inventory drifted")
    executes = [node for node in calls if _call_name(node) == "execute"]
    unmatched_executes = list(executes)
    for expected in EXPECTED_EXECUTE_EXPRESSIONS:
        for index, execute in enumerate(unmatched_executes):
            if _same_expression(execute, expected):
                del unmatched_executes[index]
                break
        else:
            raise AssertionError("GraphQL example DuckDB statement authority drifted")
    if unmatched_executes:
        raise AssertionError("GraphQL example gained an unapproved DuckDB statement")
    getpass_calls = [node for node in calls if _call_name(node) == "getpass"]
    if len(getpass_calls) != 1 or not _same_expression(
        getpass_calls[0], "getpass.getpass('Short-lived GitHub token: ')"
    ):
        raise AssertionError("GraphQL example credential prompt drifted")
    if any(isinstance(node, ast.Assert) for node in ast.walk(tree)):
        raise AssertionError("GraphQL example can expose assertion data")
    if any(
        isinstance(node, (ast.NamedExpr, ast.AugAssign, ast.Delete))
        for node in ast.walk(tree)
    ):
        raise AssertionError("GraphQL example gained an untracked write form")
    forbidden_binding_nodes = (ast.ClassDef, ast.Global, ast.Nonlocal, ast.Match)
    if any(isinstance(node, forbidden_binding_nodes) for node in ast.walk(tree)):
        raise AssertionError("GraphQL example gained an untracked binding scope")
    type_alias = getattr(ast, "TypeAlias", None)
    if type_alias is not None and any(isinstance(node, type_alias) for node in ast.walk(tree)):
        raise AssertionError("GraphQL example gained an untracked type binding")
    binding_targets: list[ast.expr] = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Assign):
            binding_targets.extend(node.targets)
        elif isinstance(node, ast.AnnAssign):
            binding_targets.append(node.target)
        elif isinstance(node, (ast.For, ast.AsyncFor, ast.comprehension)):
            binding_targets.append(node.target)
        elif isinstance(node, (ast.With, ast.AsyncWith)):
            binding_targets.extend(
                item.optional_vars
                for item in node.items
                if item.optional_vars is not None
            )
    if any(not isinstance(target, ast.Name) for target in binding_targets):
        raise AssertionError("GraphQL example gained a non-name binding target")
    annotated = {
        node.target.id: (ast.unparse(node.annotation), ast.unparse(node.value))
        for node in ast.walk(tree)
        if isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name)
    }
    if annotated != EXPECTED_ANNOTATED_ASSIGNMENTS:
        raise AssertionError("GraphQL example annotated assignment inventory drifted")
    name_stores = Counter(
        node.id
        for node in ast.walk(tree)
        if isinstance(node, ast.Name)
        and isinstance(node.ctx, ast.Store)
    )
    if dict(name_stores) != EXPECTED_NAME_STORE_COUNTS:
        raise AssertionError("GraphQL example closed binding inventory drifted")
    functions = [
        node
        for node in ast.walk(tree)
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
    ]
    function_arguments = {
        node.name: tuple(
            argument.arg
            for argument in (
                *node.args.posonlyargs,
                *node.args.args,
                *node.args.kwonlyargs,
            )
        )
        for node in functions
        if node.args.vararg is None and node.args.kwarg is None
    }
    if (
        function_arguments != EXPECTED_FUNCTION_ARGUMENTS
        or len(functions) != len(EXPECTED_FUNCTION_ARGUMENTS)
        or any(getattr(node, "type_params", ()) for node in functions)
        or any(isinstance(node, ast.Lambda) for node in ast.walk(tree))
    ):
        raise AssertionError("GraphQL example function binding inventory drifted")
    handlers = Counter(
        (ast.unparse(node.type) if node.type is not None else None, node.name)
        for node in ast.walk(tree)
        if isinstance(node, ast.ExceptHandler)
    )
    if handlers != Counter({("Exception", None): 2, ("SystemExit", None): 1}):
        raise AssertionError("GraphQL example exception binding inventory drifted")

    fetchalls = [
        node
        for node in ast.walk(tree)
        if isinstance(node, ast.Call) and _call_name(node) == "fetchall"
    ]
    if len(fetchalls) != 2:
        raise AssertionError("GraphQL example fetchall inventory drifted")
    fetchall_sources = [ast.get_source_segment(source, node) or "" for node in fetchalls]
    if not all(
        f"statements[{index + 1}]" in fetchall_sources[index] for index in range(2)
    ):
        raise AssertionError("GraphQL example can fetch live repository rows")

    prints = [
        node
        for node in ast.walk(tree)
        if isinstance(node, ast.Call) and _call_name(node) == "print"
    ]
    if len(prints) != 1 or len(prints[0].args) != 1:
        raise AssertionError("GraphQL example output inventory drifted")
    dump_call = prints[0].args[0]
    if (
        not isinstance(dump_call, ast.Call)
        or _call_name(dump_call) != "dumps"
        or not dump_call.args
        or not isinstance(dump_call.args[0], ast.Dict)
    ):
        raise AssertionError("GraphQL example output is not one fixed JSON object")
    output = dump_call.args[0]
    keys = [
        key.value
        for key in output.keys
        if isinstance(key, ast.Constant) and isinstance(key.value, str)
    ]
    if keys != EXPECTED_OUTPUT_KEYS:
        raise AssertionError("GraphQL example public output keys drifted")
    values_by_key = dict(zip(keys, output.values, strict=True))
    for key, expected in EXPECTED_OUTPUT_EXPRESSIONS.items():
        if not _same_expression(values_by_key[key], expected):
            raise AssertionError(
                f"GraphQL example public output value {key} lost safe provenance"
            )

    assignments = _assignments(tree)
    for name, expected in EXPECTED_ASSIGNMENT_EXPRESSIONS.items():
        _require_exact_assignment(assignments, name, expected)
    token_assignments = Counter(
        ast.dump(value, include_attributes=False)
        for value in assignments.get("token", [])
    )
    expected_token_assignments = Counter(
        ast.dump(ast.parse(value, mode="eval").body, include_attributes=False)
        for value in ("''", "''", "''", "read_token()", "getpass.getpass('Short-lived GitHub token: ')")
    )
    if token_assignments != expected_token_assignments:
        raise AssertionError("GraphQL example credential assignment inventory drifted")

    for name, expected in EXPECTED_CONSTANTS.items():
        values = assignments.get(name, [])
        if len(values) != 1:
            raise AssertionError(f"GraphQL example {name} inventory drifted")
        try:
            actual = ast.literal_eval(values[0])
        except (ValueError, TypeError):
            raise AssertionError(f"GraphQL example {name} is not fixed data") from None
        if actual != expected:
            raise AssertionError(f"GraphQL example {name} contract drifted")

    extension_values = assignments.get("extension", [])
    if len(extension_values) != 1 or not isinstance(extension_values[0], ast.Call):
        raise AssertionError("GraphQL example extension provenance drifted")
    extension_source = ast.get_source_segment(source, extension_values[0]) or ""
    if (
        _call_name(extension_values[0]) != "fetchone"
        or " ".join(extension_source.split()).find(EXPECTED_EXTENSION_QUERY) < 0
    ):
        raise AssertionError("GraphQL example extension identity query drifted")

    required_guards = (
        "extension != EXPECTED_EXTENSION",
        "schema != EXPECTED_SCHEMA",
        (
            "result_schema != EXPECTED_RESULT_SCHEMA or "
            "completion != (True, True, True)"
        ),
    )
    if_tests = [node.test for node in ast.walk(tree) if isinstance(node, ast.If)]
    if any(not any(_same_expression(test, guard) for test in if_tests) for guard in required_guards):
        raise AssertionError("GraphQL example safe output guard drifted")

    safe_dynamic_exits = ("main()",)
    bare_raises = [node for node in ast.walk(tree) if isinstance(node, ast.Raise) and node.exc is None]
    guarded_reraises = [
        handler
        for handler in ast.walk(tree)
        if isinstance(handler, ast.ExceptHandler)
        and isinstance(handler.type, ast.Name)
        and handler.type.id == "SystemExit"
        and len(handler.body) == 1
        and handler.body[0] in bare_raises
    ]
    if len(bare_raises) != 1 or len(guarded_reraises) != 1:
        raise AssertionError("GraphQL example SystemExit containment drifted")
    for node in ast.walk(tree):
        if not isinstance(node, ast.Raise):
            continue
        if node.exc is None and node in bare_raises:
            continue
        if (
            not isinstance(node.exc, ast.Call)
            or _call_name(node.exc) != "SystemExit"
            or len(node.exc.args) != 1
        ):
            raise AssertionError("GraphQL example gained a non-contract exception path")
        argument = node.exc.args[0]
        if not isinstance(argument, ast.Constant) and not any(
            _same_expression(argument, expected) for expected in safe_dynamic_exits
        ):
            raise AssertionError("GraphQL example failure output can expose dynamic data")

    forbidden = (
        "--token",
        "GITHUB_TOKEN",
        "os.environ",
        '"repository_count"',
        '"row_count"',
        '"cursor"',
    )
    if any(value in source for value in forbidden):
        raise AssertionError("GraphQL example exposes a credential or row-data path")
    if "getpass.getpass" not in source:
        raise AssertionError("GraphQL example lost interactive credential input")


def validate_privacy_mutation_oracles(source: str) -> None:
    """Prove that allowed output keys cannot disguise sensitive values."""

    mutations = (
        (
            "approved function name rebound",
            "        token = read_token()\n",
            "        token = read_token()\n        len = print\n",
        ),
        (
            "annotated subscript binding",
            "        schema = [(row[0], row[1]) for row in described]\n",
            (
                "        schema = [(row[0], row[1]) for row in described]\n"
                "        schema[0]: tuple[str, str] = (token, token)\n"
            ),
        ),
        (
            "loop subscript binding",
            "        schema = [(row[0], row[1]) for row in described]\n",
            (
                "        schema = [(row[0], row[1]) for row in described]\n"
                "        for schema[0] in [(token, token)]:\n"
                "            pass\n"
            ),
        ),
        (
            "exception credential binding",
            "        except Exception:\n",
            "        except Exception as token:\n",
        ),
        (
            "assignment expression rebinding",
            '    sql_path = pathlib.Path(__file__).with_name("viewer-repository-metrics.sql")\n',
            (
                '    sql_path = pathlib.Path(__file__).with_name("viewer-repository-metrics.sql")\n'
                "    (sql_path := sql_path)\n"
            ),
        ),
        (
            "augmented assignment rebinding",
            '    sql_path = pathlib.Path(__file__).with_name("viewer-repository-metrics.sql")\n',
            (
                '    sql_path = pathlib.Path(__file__).with_name("viewer-repository-metrics.sql")\n'
                '    sql_path += ""\n'
            ),
        ),
        (
            "authority deletion",
            '    sql_path = pathlib.Path(__file__).with_name("viewer-repository-metrics.sql")\n',
            (
                '    sql_path = pathlib.Path(__file__).with_name("viewer-repository-metrics.sql")\n'
                "    del sql_path\n"
            ),
        ),
        (
            "annotated authority rebinding",
            '    sql_path = pathlib.Path(__file__).with_name("viewer-repository-metrics.sql")\n',
            (
                '    sql_path = pathlib.Path(__file__).with_name("viewer-repository-metrics.sql")\n'
                "    sql_path: object = sql_path\n"
            ),
        ),
        (
            "runtime schema subscript mutation",
            "        schema = [(row[0], row[1]) for row in described]\n",
            (
                "        schema = [(row[0], row[1]) for row in described]\n"
                "        schema[0] = (token, token)\n"
            ),
        ),
        (
            "approved call name rebound to output",
            "from __future__ import annotations\n",
            (
                "from __future__ import annotations\n\n"
                "from builtins import print as len\n"
            ),
        ),
        (
            "unreviewed SQL file selected",
            'with_name("viewer-repository-metrics.sql")',
            'with_name("unreviewed.sql")',
        ),
        (
            "persistent DuckDB database selected",
            'duckdb.connect(config={"allow_unsigned_extensions": "true"})',
            (
                'duckdb.connect("persistent.db", '
                'config={"allow_unsigned_extensions": "true"})'
            ),
        ),
        (
            "credential written to stderr",
            "        token = read_token()\n",
            (
                "        token = read_token()\n"
                '        __import__("sys").stderr.write(token)\n'
            ),
        ),
        (
            "credential sent to logging",
            "        token = read_token()\n",
            (
                "        token = read_token()\n"
                '        __import__("logging").warning(token)\n'
            ),
        ),
        (
            "repository result written to stderr",
            "        completion = result.fetchone()\n",
            (
                "        completion = result.fetchone()\n"
                '        __import__("sys").stderr.write(str(completion))\n'
            ),
        ),
        (
            "GraphQL document written to stderr",
            "        token = read_token()\n",
            (
                "        token = read_token()\n"
                '        __import__("sys").stderr.write(document)\n'
            ),
        ),
        (
            "cursor written to stderr",
            "        token = read_token()\n",
            (
                "        token = read_token()\n"
                '        __import__("sys").stderr.write(cursor)\n'
            ),
        ),
        (
            "provider error written to stderr",
            (
                "    except Exception:\n"
                '        raise SystemExit("GraphQL repository validation failed safely") from None\n'
            ),
            (
                "    except Exception as error:\n"
                '        __import__("sys").stderr.write(str(error))\n'
                '        raise SystemExit("GraphQL repository validation failed safely") from None\n'
            ),
        ),
        (
            "credential value under an allowed key",
            '"required_values_present": True,',
            '"required_values_present": token,',
        ),
        (
            "GraphQL document under an allowed key",
            '"required_values_present": True,',
            '"required_values_present": document,',
        ),
        (
            "cursor under an allowed key",
            '"required_values_present": True,',
            '"required_values_present": cursor,',
        ),
        (
            "repository query under the artifact key",
            '"artifact": artifact.name,',
            '"artifact": connection.execute("SELECT full_name FROM repositories").fetchall(),',
        ),
        (
            "provider error text in failure output",
            '"GraphQL repository validation failed safely"',
            "str(error)",
        ),
        (
            "live result bypassing the fixed completion tuple",
            "completion = result.fetchone()",
            "completion = connection.execute(statements[3]).fetchone()",
        ),
    )
    for label, accepted, counterexample in mutations:
        mutated = source.replace(accepted, counterexample, 1)
        if mutated == source:
            raise AssertionError(f"GraphQL privacy mutation source drifted: {label}")
        try:
            validate_runner_source(mutated)
        except AssertionError:
            continue
        raise AssertionError(f"GraphQL privacy validator accepted {label}")


def validate_sql_source(source: str) -> None:
    """Require offline inspection and a boolean-only live result surface."""

    normalized_lines = []
    for line in source.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("--"):
            continue
        if "--" in stripped or "/*" in stripped or "*/" in stripped:
            raise AssertionError("GraphQL example SQL contains an inline comment")
        normalized_lines.append(stripped)
    normalized = " ".join(normalized_lines)
    statements = [value.strip() for value in normalized.split(";") if value.strip()]
    if len(statements) != 4:
        raise AssertionError("GraphQL example SQL statement inventory drifted")
    required = (
        "CALL cuac_load_connector(",
        "package_root := '/absolute/path/to/cuac/connectors/github'",
        "DESCRIBE SELECT id, full_name, owner_login, stars, primary_language, private, archived, updated_at",
        "EXPLAIN SELECT full_name, stars, primary_language, updated_at",
        "github_viewer_repository_metrics(",
        "secret := 'github_default'",
        "WHERE archived = FALSE",
        "ORDER BY stars DESC, full_name",
        "LIMIT 10",
        "AS required_values_present",
        "AS local_limit_respected",
        "AS local_filter_respected",
    )
    if any(fragment not in normalized for fragment in required):
        raise AssertionError("GraphQL example SQL lost an accepted product fact")
    if (
        "SELECT *" in normalized
        or " AS repository_count" in normalized
        or "cuac_scan" in normalized
    ):
        raise AssertionError("GraphQL live SQL exposes rows or a raw count")


def validate_help(stdout: str) -> None:
    if "--token" in stdout or "GITHUB_TOKEN" in stdout:
        raise AssertionError("GraphQL example help exposes a credential argument")
