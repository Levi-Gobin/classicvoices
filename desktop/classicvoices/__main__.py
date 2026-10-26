import sys


def main():
    if "--register-sapi" in sys.argv:  # the elevated helper the app starts (Windows)
        from . import system
        sys.exit(system.register_system_voices_elevated())
    from . import ui
    ui.main()


if __name__ == "__main__":
    main()
