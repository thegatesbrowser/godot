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


        # CHROMIUM SANDBOXING

        env.Replace(CC = "clang-cl")
        env.Replace(CXX = "clang-cl")
        
        # Enable required instruction sets for libwebp
        # TODO: remove this as it was discussed here https://github.com/godotengine/godot/pull/36580/files
        env.Append(CCFLAGS=['-mssse3'])
        env.Append(CCFLAGS=["-msse4.1"])

        # Disable assembly optimizations in R128 library and use standard C implementation
        env.Prepend(CPPDEFINES=['R128_STDC_ONLY'])
        
        # Add chromium include paths
        env.Prepend(CPPPATH=[
            'C:/src/chromium/src/',
            'C:/src/chromium/src/out/sandbox_build/gen/',
            'C:/src/chromium/src/buildtools/third_party/libc++/',
            'C:/src/chromium/src/third_party/perfetto/include/',
            'C:/src/chromium/src/out/sandbox_build/gen/third_party/perfetto/build_config/',
            'C:/src/chromium/src/out/sandbox_build/gen/third_party/perfetto/',
            'C:/src/chromium/src/base/allocator/partition_allocator/src/',
            'C:/src/chromium/src/out/sandbox_build/gen/base/allocator/partition_allocator/src/',
            'C:/src/chromium/src/third_party/abseil-cpp/',
            'C:/src/chromium/src/third_party/boringssl/src/include/',
            'C:/src/chromium/src/third_party/protobuf/src/'
        ])

        # Add chromium library paths
        env.Append(LIBPATH=[
            "C:/src/chromium/src/out/sandbox_build",
            "C:/src/chromium/src/out/sandbox_build/obj/sandbox/win",
            "C:/src/chromium/src/out/sandbox_build/obj/base/win",
            "C:/src/chromium/src/out/sandbox_build/win_clang_x64_for_rust_host_build_tools/rustlib/windows_x86_64_msvc_lib_v0_52",
            "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.43.34808/lib/x64",
            "C:/src/chromium/src/out/sandbox_build/obj/sandbox/win",
            "C:/src/chromium/src/out/sandbox_build/obj/sandbox"
        ])

        env.Append(LINKFLAGS=[
            "base.dll.lib",
            "sandbox.lib",
            "libc++.dll.lib",
            "pe_image.lib",
            "third_party_abseil-cpp_absl.dll.lib",
            "third_party_perfetto_libperfetto.dll.lib",
            "third_party_boringssl.dll.lib",
            "third_party_zlib.dll.lib",
            "cppgen_plugin.lib",
            "protozero_plugin.lib",
            "base_allocator_partition_allocator_src_partition_alloc_raw_ptr.dll.lib",
            "base_allocator_partition_allocator_src_partition_alloc_allocator_shim.dll.lib",
            "base_allocator_partition_allocator_src_partition_alloc_allocator_core.dll.lib",
            "base_allocator_partition_allocator_src_partition_alloc_allocator_base.dll.lib",
            "windows.0.52.0.lib",
            "delayimp.lib",
            "common.lib",
            "service_resolver.lib"
        ])

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
