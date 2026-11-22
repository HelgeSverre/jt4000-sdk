"""Shared entry point for the tools: turn expected failures into one-line errors.

    if __name__ == "__main__":
        cli.run(main)

A missing file, an unplugged device or a dropped connection ends with a short message and
exit status 1 instead of a traceback. Set JT_DEBUG=1 to get the traceback back.
"""

from __future__ import annotations

import os
import sys
from collections.abc import Callable


def fail(message: str, code: int = 1) -> None:
    sys.exit(f"error: {message}" if code else message)


def run(main: Callable[[], object]) -> None:
    if os.environ.get("JT_DEBUG"):
        main()
        return
    script = os.path.relpath(sys.argv[0])
    try:
        main()
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        sys.exit(130)
    except BrokenPipeError:
        sys.exit(1)  # output was piped into something that stopped reading
    except ModuleNotFoundError as e:
        fail(
            f"the Python module '{e.name}' is not installed. "
            f"Run it as: uv run --with {e.name} {script} ..."
        )
    except FileNotFoundError as e:
        fail(f"{e.filename or e}: no such file or directory")
    except PermissionError as e:
        fail(f"{e.filename or e}: permission denied")
    except IsADirectoryError as e:
        fail(f"{e.filename or e}: is a directory, expected a file")
    except (OSError, RuntimeError, EOFError) as e:
        # OSError covers serial ports, MIDI ports and the network.
        fail(str(e) or type(e).__name__)
    except Exception as e:  # noqa: BLE001  a bug, or a reply nobody has seen yet
        fail(
            f"unexpected {type(e).__name__}: {e}\n"
            f"       Run again with JT_DEBUG=1 for the traceback, and please report it.",
            code=2,
        )
