# This prototype needs a marked Studio copy and an explicit rebuilt loader.
# Defining a help target also prevents recursive example source discovery from
# treating the standalone regression tests as part of a native mod.
add_custom_target(${CURRENT_MOD_NAME}
    COMMAND ${CMAKE_COMMAND} -E echo
        "Workspace is an opt-in guarded prototype. Follow examples/workspace/README.md."
    VERBATIM
)
