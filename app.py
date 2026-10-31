"""Entry point of the packaged program.

    serve       the web UI and scheduler; this is what the service runs
    uninstall   Windows only: remove the program
    (nothing)   Windows: the small control window; Linux: same as serve
"""

import sys


def main():
    verb = sys.argv[1] if len(sys.argv) > 1 else ""
    if verb == "serve" or (not verb and sys.platform != "win32"):
        import server
        server.main()
        return 0
    if sys.platform == "win32":
        import winsetup
        return winsetup.installed_main(sys.argv[1:])
    print(__doc__.strip(), file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
