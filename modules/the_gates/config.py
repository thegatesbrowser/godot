import os
import sys


def can_build(env, platform):
    if not env["arch"] or env.msvc:
        return True

    if env["platform"] == "windows":
        pkgconf_error = os.system("pkg-config --version > NUL")
    else:
        pkgconf_error = os.system("pkg-config --version > /dev/null")

    if pkgconf_error:
        print("Error: pkg-config not found. Aborting.")
        return False

    return True


def configure(env):
    if not env["arch"]:
        return

    if env["platform"] == "linuxbsd":
        if os.system("pkg-config --exists libseccomp"):
            print("Error: Seccomp library not found. Aborting.")
            sys.exit(255)
        else:
            env.ParseConfig("pkg-config libseccomp --cflags --libs")
            print("Linking Seccomp")
