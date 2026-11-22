idf_component_register(
    SRCS "bench_main.c"
         "../../../recomp/hot_func.c"
         "../../../recomp/delay_func.c"
    INCLUDE_DIRS "." "../../../recomp"
)
