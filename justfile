@default:
    ninja --quiet -Cb
    b/sdb -l
    b/sdb -den str
@t *a:
    ninja --quiet -Cb
    b/sdb {{a}}
@s *a:
    ninja --quiet -Cb
    strace b/sdb {{a}}
@v *a:
    ninja --quiet -Cb
    valgrind --leak-check=full --track-origins=yes -q b/sdb -den str
