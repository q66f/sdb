@test *a:
    ninja --quiet -Cb
    b/sdb {{a}}
@st *a:
    ninja --quiet -Cb
    strace b/sdb {{a}}
@val *a:
    ninja --quiet -Cb
    valgrind --leak-check=full --track-origins=yes -q b/sdb {{a}}
