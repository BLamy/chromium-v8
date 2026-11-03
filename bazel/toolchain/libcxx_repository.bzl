"""Stub repository rule: workerd uses the system/toolchain libc++, so @libcxx
is an empty shim that satisfies the dep in bazel/defs.bzl without pulling in
any third_party sources."""

def _libcxx_repository_impl(ctx):
    ctx.file("BUILD.bazel", """
package(default_visibility = ["//visibility:public"])

# Empty stub - the real libc++ is provided by the toolchain.
cc_library(name = "libc++")
cc_library(name = "libc++abi")
""")

libcxx_repository = repository_rule(
    implementation = _libcxx_repository_impl,
    local = True,
)
