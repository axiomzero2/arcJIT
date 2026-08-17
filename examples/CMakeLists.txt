# arcJIT — examples/CMakeLists.txt

# Examples are simple Arc programs that the CLI can run. For now, they're
# just data files — no compilation needed.

file(COPY
    demo_add3.arc
    demo_loop.arc
    demo_call.arc
    DESTINATION ${CMAKE_CURRENT_BINARY_DIR}
)
