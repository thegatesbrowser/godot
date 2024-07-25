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

    if env.msvc:
        # TODO: fix not msvc windows build
        
        # 1. Build libzmq https://www.youtube.com/watch?v=OiGf9T_TPa8
        # 2. Fix linking mismatch https://stackoverflow.com/questions/28887001/lnk2038-mismatch-detected-for-runtimelibrary-value-mt-staticrelease-doesn
        # 3. Place inside C:/Program Files (x86)/ZeroMQ/
        env.Prepend(CPPDEFINES=["ZMQ_STATIC"])
        env.Prepend(CPPPATH=["C:/Program Files (x86)/ZeroMQ/include"])
        env.Append(LIBPATH=["C:/Program Files (x86)/ZeroMQ/lib"])
        env.Append(LINKFLAGS=["libzmq-v143-mt-s-4_3_5.lib"])
        print("Linking ZeroMQ statically")

    elif env["platform"] == "linuxbsd":
        if os.system("pkg-config --exists libzmq"):
            print("Error: ZeroMQ librarie not found. Aborting.")
            sys.exit(255)
        else:
            env.ParseConfig("pkg-config libzmq --cflags --libs --static")
            print("Linking ZeroMQ statically")

        if os.system("pkg-config --exists libseccomp"):
            print("Error: Seccomp librarie not found. Aborting.")
            sys.exit(255)
        else:
            env.ParseConfig("pkg-config libseccomp --cflags --libs")
            print("Linking Seccomp")

    else:
        # MacOS

        # 1. Build zeromq with cmake and Xcode both arm64 (macOS 11.0+) and x86_64 (macOS 10.13+) https://github.com/zeromq/libzmq
        # 2. Link them with lipo into universal library
        # 3. Replace libzmq.dylib that pkg-config returns
        if os.system("pkg-config --exists libzmq"):
            print("Error: ZeroMQ librarie not found. Aborting.")
            sys.exit(255)
        else:
            # Linked dynamically
            env.ParseConfig("pkg-config libzmq --cflags --libs")
            print("Linking ZeroMQ dynamically")
